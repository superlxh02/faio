/**
 * @file faio/detail/runtime/multi_thread/scheduler/local_scheduler.hpp
 * @brief 运行时调度、线程协调与定时驱动：本地任务获取、公平性与窃取。
 * @details 运行时按所属线程区分私有状态与跨线程发布状态。任务接纳先登记完成责任，关闭先停止接纳再排空；休眠登记后复查工作以避免丢失唤醒。
 */
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
/// @brief 每个 worker 一个本地调度器：管理本地队列、公平性、批量拉取、窃取与休眠转换。
/// 所属线程操作私有状态，其他线程只能经 domain_scheduler 窃取其环形队列。
class local_scheduler {
 public:
  /// @brief 借用共享调度域并登记本地队列；底层唤醒端必须存活到本对象注销之后。
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

  /// @brief 所有 worker 的事件循环已通过退出屏障后才调用此析构。
  ~local_scheduler() { domain_.unregister_local(worker_id_); }

  /// @brief 把本地队列作为线程绑定状态；通用调度入口据此直接内联本地入队。
  domain_scheduler::local_queue_type& local_state() noexcept { return queue_; }

  /// @brief 接收 I/O/定时器完成的就绪协程，批量通知在 flush_ready 后统一执行。
  void enqueue_ready(std::coroutine_handle<> task) {
    queue_.push_back(task, domain_.global_queue());
  }

  /// @brief 一轮完成事件发布完毕后按需唤醒同伴，避免每个 CQE 都执行唤醒判断。
  void flush_ready() noexcept {
    /// @brief IO批次可同时拥有一个私有快速任务和FIFO任务；可窃取项一条就需要通知。
    /// FIFO溢出或已被同伴取空时，全局待处理项仍保持同一次批次的通知责任。
    if (!searching_ && (!queue_.empty() || !domain_.global_queue().empty()))
      /// @brief 成功发布可转交工作后按共享休眠状态通知一个同伴，避免任务无人搜索。
      domain_.wake_up_one();
  }

  /// @brief 获取下一任务；周期性检查全局队列，防止连续本地工作饿死外部提交。
  /// @param tick 当前调度轮次，用于全局队列公平性检查。
  /// @param force_global 是否补做之前到期但被其他执行来源推迟的全局检查。
  /// @return 当前线程独占领取的句柄；全部已检查来源为空时返回 std::nullopt。
  std::optional<std::coroutine_handle<>> next_task(std::uint32_t tick, bool force_global = false) {
    /// @brief 全局/批量/无任务等路径默认不是连续私有来源。
    last_task_from_fast_ = false;
    /// @brief 周期到期或已有待处理的全局检查时，先给外部提交一次领取机会。
    if (force_global || tick % global_queue_interval_ == 0) {
      /// @brief 全局公平选择获得完整新额度。
      if (auto task = domain_.global_queue().try_pop())
        return task;
      /// @brief 全局无任务时仍精确区分本地FIFO与fast。
      return next_local_task();
    }
    /// @brief 优先尝试私有快速槽与本地 FIFO；来源标记决定本次执行是否补充协作预算。
    if (auto task = next_local_task())
      return task;
    /// @brief 使用固定数组作为输出缓冲区，不在每次批量拉取时分配 vector。
    std::array<std::coroutine_handle<>, LOCAL_QUEUE_CAPACITY / 2> batch;
    /// @brief 按本地可复用空间限制批量拉取，固定栈数组避免每批临时分配。
    const auto capacity = std::min(queue_.remaining_capacity(), batch.size());
    /// @brief 在一次全局锁内取回有界批次，减轻多个工作线程争抢全局容器的成本。
    const auto count = domain_.global_queue().try_pop_batch(std::span{batch}.first(capacity));
    /// @brief 批量拉取没有取得任务时，后续不得沿用前一条快速执行链的预算。
    if (count == 0) {
      /// @brief 真正无就绪结果不可把旧余额跨空闲/IO等待带到后续fast。
      interrupt_execution_chain();
      return std::nullopt;
    }
    /// @brief 把除直接执行项之外的批次发布到本地 FIFO，其他工作线程可继续窃取。
    queue_.push_back_batch(std::span{batch}.first(count - 1));
    /// @brief 全局批次立即返回项同样是fresh，FIFO剩余项也各自fresh。
    return batch[count - 1];
  }

  /// @brief 空闲时领取搜索名额并尝试窃取，失败后再检查一次全局队列。
  /// @return 窃取批次的直接执行项或全局回退项；未取得任务时返回 std::nullopt。
  /// @details 只有共享状态机允许的搜索线程扫描同伴队列；随机起点由本线程
  /// 私有状态生成。取得任务时不在此恢复协程，实际执行边界再归还搜索名额。
  std::optional<std::coroutine_handle<>> steal_task() {
    /// @brief 窃取和其全局回退永远开始新的执行epoch。
    last_task_from_fast_ = false;
    /// @brief 尚未登记为搜索者时先争取共享搜索名额，限制并发窃取造成的缓存争用。
    if (!searching_)
      /// @brief 只有取得名额的线程才扫描其他本地队列，未取得者仍可消费全局提交。
      searching_ = domain_.start_searching();
    /// @brief 只有已经登记共享搜索名额的线程才需要参与对应的窃取或名额归还流程。
    if (searching_) {
      /// @brief 每个本地调度器独有的轻量状态，不使用共享随机数锁或 random_device
      /// 热路径。
      random_state_ ^= random_state_ << 13;
      /// @brief 混合高低位，使相邻工作线程不会长期从同一受害者开始扫描。
      random_state_ ^= random_state_ >> 7;
      /// @brief 完成本轮随机状态更新，随后按工作线程数量取模。
      random_state_ ^= random_state_ << 17;
      /// @brief 只有真实领取后才由worker建立执行scope。
      if (auto task = domain_.steal_into(worker_id_, queue_, random_state_ % domain_.num_workers()))
        return task;
    }
    /// @brief 远程回退仍不是fast链。
    if (auto task = domain_.global_queue().try_pop())
      return task;
    /// @brief 未找到工作不延续旧poll余额。
    interrupt_execution_chain();
    return std::nullopt;
  }

  /** @brief
   * worker真正resume时领取本次来源对应预算；scope退出保存剩余并还原外层TLS。 */
  auto begin_execution() noexcept { return execution_budget_.begin(last_task_from_fast_); }

  /** @brief
   * drive/时间预算drive/休眠结束连续fast链，已选中但未执行的task仍会fresh。 */
  void interrupt_execution_chain() noexcept { execution_budget_.interrupt(); }

  /// @brief 真正执行任务前取消搜索标识，搜索计数不包含正在运行用户代码的线程。
  void before_execute() noexcept {
    /// @brief 只有已经登记共享搜索名额的线程才需要参与对应的窃取或名额归还流程。
    if (searching_) {
      /// @brief 更新线程私有搜索状态，后续共享计数转换由调度域成对完成。
      searching_ = false;
      /// @brief 归还一个共享搜索名额；若为最后搜索者，调度域还须检查可转交工作。
      domain_.stop_searching();
    }
  }

  /// @brief 所属线程检查本地快速槽和环形队列，不读取其他线程私有快速槽。
  bool has_local_task() const noexcept { return !queue_.empty_local(); }

  /// @brief 仅所属线程查询；未领取搜索名额的线程直接休眠，减少空闲扫描争用。
  bool is_searching() const noexcept { return searching_; }

  /// @brief 入睡登记后再次检查本地和全局工作，构成防丢失唤醒的关键握手。
  bool has_ready_task() const noexcept {
    return has_local_task() || !domain_.global_queue().empty();
  }

  /// @brief 准备进入底层等待；调用者必须在登记之后再次检查 has_ready_task。
  bool prepare_sleep() {
    if (has_local_task())
      return false;
    /// @brief 在共享状态机登记休眠并撤销工作或搜索计数，随后调用方仍须重新检查就绪工作。
    domain_.prepare_sleep(worker_id_, searching_);
    /// @brief 更新线程私有搜索状态，后续共享计数转换由调度域成对完成。
    searching_ = false;
    return true;
  }

  /// @brief I/O 返回或二次检查发现任务后撤销休眠，恢复与共享计数一致的搜索状态。
  bool finish_sleep(bool external_ready = false) {
    /// @brief 休眠登记后发现本地、全局或外部工作，先撤销本线程休眠记录再回到执行循环。
    if (external_ready || has_ready_task()) {
      /// @brief 先移除自身休眠记录，恢复真实工作/搜索计数。
      searching_ =
          !domain_.cancel_sleep(worker_id_);
      /// @brief IO 自然返回或登记后早期重查都走这里；通知候选不能再包含尚未注销的自身。
      /// 已被远程通知摘除时 searching_=true，flush
      /// 延用现有搜索者责任，不重复通知。
      /// @brief 真实 FIFO/全局批次仍负责唤醒同伴；单个私有快速任务不通知其他线程。
      flush_ready();

      return true;
    }
    /// @brief 内核事件返回不一定代表调度唤醒，仍在休眠集合时继续等待。
    if (domain_.still_sleeping(worker_id_))
      return false;
    /// @brief 唤醒方已摘除休眠记录并增加搜索计数。
    searching_ = true;
    return true;
  }

 private:
  /// @brief 所属线程在耗尽边界先安置等待的fast，防止自让出FIFO反复补预算却永远排斥它。
  std::optional<std::coroutine_handle<>> next_local_task() {
    /// @brief 连续快速执行链已耗尽协作预算时，先把仍等待的私有任务转为可公平领取的 FIFO。
    if (execution_budget_.exhausted()) {
      /// @brief 入队前只观察FIFO，私有fast本身不是另一条工作。
      const bool another_ready = !queue_.empty();
      /// @brief 降级后可窃取；已有FIFO/global才需要额外唤醒并行执行者。
      if (queue_.demote_fast(domain_.global_queue())
          && (another_ready || !domain_.global_queue().empty()))
        /// @brief 成功发布可转交工作后按共享休眠状态通知一个同伴，避免任务无人搜索。
        domain_.wake_up_one();
    }
    /// @brief 降级项与已有续体按原FIFO消费，不再重复禁用同一fast。
    auto task = queue_.try_pop_local();
    /// @brief 不猜句柄身份或promise布局。
    last_task_from_fast_ = task && queue_.last_pop_from_fast();
    /// @brief 包括全局周期分支直接返回nullopt的路径。
    if (!task)
      /// @brief 没有实际选中工作或经历 IO 等待时中断旧执行链，后续恢复重新取得预算。
      interrupt_execution_chain();
    return task;
  }

  local_execution_budget execution_budget_;  ///< worker私有连续链，无frame字段和跨线程共享。
  bool last_task_from_fast_{false};  ///< 当前选中任务来源，driver可只中断epoch不修改句柄。
  domain_scheduler& domain_;  ///< 借用整个 runtime 的共享调度域。
  std::size_t worker_id_;  ///< 本地调度器对应的线程编号。
  std::uint32_t global_queue_interval_;  ///< 全局队列公平性检查间隔，配置保证非零。
  std::uint64_t random_state_;  ///< 所属线程私有的窃取起点生成状态。
  domain_scheduler::local_queue_type queue_;  ///< 本地环形队列与私有快速恢复槽。
  bool searching_{false};  ///< 本线程是否已领取共享搜索名额。
};

static_assert(ready_sink<local_scheduler>);
}
#endif
