#ifndef FAIO_DETAIL_RUNTIME_MULTI_THREAD_SCHEDULER_LOCAL_SCHEDULER_HPP
#define FAIO_DETAIL_RUNTIME_MULTI_THREAD_SCHEDULER_LOCAL_SCHEDULER_HPP

#include "faio/detail/runtime/multi_thread/scheduler/domain_scheduler.hpp"
#include "faio/detail/runtime/multi_thread/scheduler/local_execution_budget.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace faio::runtime::detail {
// 每个 worker 一个本地调度器：管理本地队列、公平性、批量拉取、窃取与休眠转换。
// 所属线程操作私有状态，其他线程只能经 domain_scheduler 窃取其环形队列。
class local_scheduler {
 public:
  // 借用共享调度域并登记本地队列；底层唤醒端必须存活到本对象注销之后。
  local_scheduler(domain_scheduler& domain,
                  std::size_t worker_id,
                  std::uint32_t global_queue_interval,
                  worker_waker_ref waker)
      : domain_(domain),
        worker_id_(worker_id),
        global_queue_interval_(global_queue_interval),
        random_state_(worker_id + 1) {
    if (global_queue_interval == 0)
      throw std::invalid_argument("全局队列检查间隔必须大于零");
    domain_.register_local(worker_id_, queue_, waker);
  }

  local_scheduler(const local_scheduler&) = delete;

  local_scheduler& operator=(const local_scheduler&) = delete;

  // 所有 worker 的事件循环已通过退出屏障后才调用此析构。
  ~local_scheduler() { domain_.unregister_local(worker_id_); }

  // 把本地队列作为线程绑定状态；通用调度入口据此直接内联本地入队。
  domain_scheduler::local_queue_type& local_state() noexcept { return queue_; }

  // 接收 I/O/定时器完成的就绪协程，批量通知在 flush_ready 后统一执行。
  void enqueue_ready(std::coroutine_handle<> task) {
    queue_.push_back(task, domain_.global_queue());
  }

  // 一轮完成事件发布完毕后按需唤醒同伴，避免每个 CQE 都执行唤醒判断。
  void flush_ready() noexcept {
    // IO批次可同时拥有一个私有快速任务和FIFO任务；可窃取项一条就需要通知。
    // FIFO溢出或已被同伴取空时，全局待处理项仍保持同一次批次的通知责任。
    if (!searching_ && (!queue_.empty() || !domain_.global_queue().empty()))
      domain_.wake_up_one();
  }

  // 获取下一任务；周期性检查全局队列，防止连续本地工作饿死外部提交。
  std::optional<std::coroutine_handle<>> next_task(std::uint32_t tick) {
    last_task_from_fast_ = false;  // 全局/批量/无任务等路径默认不是连续私有来源。
    if (tick % global_queue_interval_ == 0) {
      if (auto task = domain_.global_queue().try_pop())
        return task;             // 全局公平选择获得完整新额度。
      return next_local_task();  // 全局无任务时仍精确区分本地FIFO与fast。
    }
    if (auto task = next_local_task())
      return task;
    // 使用固定数组作为输出缓冲区，不在每次批量拉取时分配 vector。
    std::array<std::coroutine_handle<>, LOCAL_QUEUE_CAPACITY / 2> batch;
    const auto capacity = std::min(queue_.remaining_capacity(), batch.size());
    const auto count = domain_.global_queue().try_pop_batch(std::span{batch}.first(capacity));
    if (count == 0) {
      interrupt_execution_chain();  // 真正无就绪结果不可把旧余额跨空闲/IO等待带到后续fast。
      return std::nullopt;
    }
    queue_.push_back_batch(std::span{batch}.first(count - 1));
    return batch[count - 1];  // 全局批次立即返回项同样是fresh，FIFO剩余项也各自fresh。
  }

  // 空闲时领取搜索名额并尝试窃取，失败后再检查一次全局队列。
  std::optional<std::coroutine_handle<>> steal_task() {
    last_task_from_fast_ = false;  // 窃取和其全局回退永远开始新的执行epoch。
    if (!searching_)
      searching_ = domain_.start_searching();
    if (searching_) {
      // 每个本地调度器独有的轻量状态，不使用共享随机数锁或 random_device
      // 热路径。
      random_state_ ^= random_state_ << 13;
      random_state_ ^= random_state_ >> 7;
      random_state_ ^= random_state_ << 17;
      if (auto task = domain_.steal_into(worker_id_, queue_, random_state_ % domain_.num_workers()))
        return task;  // 只有真实领取后才由worker建立执行scope。
    }
    if (auto task = domain_.global_queue().try_pop())
      return task;                // 远程回退仍不是fast链。
    interrupt_execution_chain();  // 未找到工作不延续旧poll余额。
    return std::nullopt;
  }

  /** @brief
   * worker真正resume时领取本次来源对应预算；scope退出保存剩余并还原外层TLS。 */
  auto begin_execution() noexcept { return execution_budget_.begin(last_task_from_fast_); }

  /** @brief
   * drive/时间预算drive/休眠结束连续fast链，已选中但未执行的task仍会fresh。 */
  void interrupt_execution_chain() noexcept { execution_budget_.interrupt(); }

  // 真正执行任务前取消搜索标识，搜索计数不包含正在运行用户代码的线程。
  void before_execute() noexcept {
    if (searching_) {
      searching_ = false;
      domain_.stop_searching();
    }
  }

  // 所属线程检查本地快速槽和环形队列，不读取其他线程私有快速槽。
  bool has_local_task() const noexcept { return !queue_.empty_local(); }

  // 仅所属线程查询；未领取搜索名额的线程直接休眠，减少空闲扫描争用。
  bool is_searching() const noexcept { return searching_; }

  // 入睡登记后再次检查本地和全局工作，构成防丢失唤醒的关键握手。
  bool has_ready_task() const noexcept {
    return has_local_task() || !domain_.global_queue().empty();
  }

  // 准备进入底层等待；调用者必须在登记之后再次检查 has_ready_task。
  bool prepare_sleep() {
    if (has_local_task())
      return false;
    domain_.prepare_sleep(worker_id_, searching_);
    searching_ = false;
    return true;
  }

  // I/O 返回或二次检查发现任务后撤销休眠，恢复与共享计数一致的搜索状态。
  bool finish_sleep() {
    if (has_ready_task()) {
      searching_ =
          !domain_.cancel_sleep(worker_id_);  // 先移除自身休眠记录，恢复真实工作/搜索计数。
      // IO 自然返回或登记后早期重查都走这里；通知候选不能再包含尚未注销的自身。
      // 已被远程通知摘除时 searching_=true，flush
      // 延用现有搜索者责任，不重复通知。
      flush_ready();  // 真实
      // FIFO/全局批次仍负责唤醒同伴；单个私有快速任务不通知其他线程。
      return true;
    }
    if (domain_.still_sleeping(worker_id_))
      return false;
    searching_ = true;  // 唤醒方已摘除休眠记录并增加搜索计数。
    return true;
  }

 private:
  // 所属线程在耗尽边界先安置等待的fast，防止自让出FIFO反复补预算却永远排斥它。
  std::optional<std::coroutine_handle<>> next_local_task() {
    if (execution_budget_.exhausted()) {
      const bool another_ready = !queue_.empty();  // 入队前只观察FIFO，私有fast本身不是另一条工作。
      if (queue_.demote_fast(domain_.global_queue())
          && (another_ready || !domain_.global_queue().empty()))
        domain_.wake_up_one();  // 降级后可窃取；已有FIFO/global才需要额外唤醒并行执行者。
    }
    auto task = queue_.try_pop_local();  // 降级项与已有续体按原FIFO消费，不再重复禁用同一fast。
    last_task_from_fast_ = task && queue_.last_pop_from_fast();  // 不猜句柄身份或promise布局。
    if (!task)
      interrupt_execution_chain();  // 包括全局周期分支直接返回nullopt的路径。
    return task;
  }

  local_execution_budget execution_budget_;   // worker私有连续链，无frame字段和跨线程共享。
  bool last_task_from_fast_{false};           // 当前选中任务来源，driver可只中断epoch不修改句柄。
  domain_scheduler& domain_;                  // 借用整个 runtime 的共享调度域。
  std::size_t worker_id_;                     // 本地调度器对应的线程编号。
  std::uint32_t global_queue_interval_;       // 全局队列公平性检查间隔，配置保证非零。
  std::uint64_t random_state_;                // 所属线程私有的窃取起点生成状态。
  domain_scheduler::local_queue_type queue_;  // 本地环形队列与私有快速恢复槽。
  bool searching_{false};                     // 本线程是否已领取共享搜索名额。
};

static_assert(ready_sink<local_scheduler>);
}  // namespace faio::runtime::detail
#endif
