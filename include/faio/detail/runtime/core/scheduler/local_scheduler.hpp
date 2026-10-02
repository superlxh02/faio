#ifndef FAIO_DETAIL_RUNTIME_CORE_SCHEDULER_LOCAL_SCHEDULER_HPP
#define FAIO_DETAIL_RUNTIME_CORE_SCHEDULER_LOCAL_SCHEDULER_HPP

#include "faio/detail/runtime/core/scheduler/domain_scheduler.hpp"
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
  local_scheduler(domain_scheduler& domain, std::size_t worker_id,
                  std::uint32_t global_queue_interval, worker_waker_ref waker)
      : domain_(domain), worker_id_(worker_id), global_queue_interval_(global_queue_interval),
        random_state_(worker_id + 1) {
    if (global_queue_interval == 0) throw std::invalid_argument("全局队列检查间隔必须大于零");
    domain_.register_local(worker_id_, queue_, waker);
  }
  local_scheduler(const local_scheduler&) = delete;
  local_scheduler& operator=(const local_scheduler&) = delete;
  // 所有 worker 的事件循环已通过退出屏障后才调用此析构。
  ~local_scheduler() { domain_.unregister_local(worker_id_); }

  // 把本地队列作为线程绑定状态；通用调度入口据此直接内联本地入队。
  domain_scheduler::local_queue_type& local_state() noexcept { return queue_; }
  // 接收 I/O/定时器完成的就绪协程，批量通知在 flush_ready 后统一执行。
  void enqueue_ready(std::coroutine_handle<> task) { queue_.push_back(task, domain_.global_queue()); }
  // 一轮完成事件发布完毕后按需唤醒同伴，避免每个 CQE 都执行唤醒判断。
  void flush_ready() noexcept {
    if (!searching_ && queue_.size() > 1) domain_.wake_up_one();
  }
  // 获取下一任务；周期性检查全局队列，防止连续本地工作饿死外部提交。
  std::optional<std::coroutine_handle<>> next_task(std::uint32_t tick) {
    if (tick % global_queue_interval_ == 0) {
      if (auto task = domain_.global_queue().try_pop()) return task;
      return queue_.try_pop_local();
    }
    if (auto task = queue_.try_pop_local()) return task;
    // 使用固定数组作为输出缓冲区，不在每次批量拉取时分配 vector。
    std::array<std::coroutine_handle<>, LOCAL_QUEUE_CAPACITY / 2> batch;
    const auto capacity = std::min(queue_.remaining_capacity(), batch.size());
    const auto count = domain_.global_queue().try_pop_batch(std::span{batch}.first(capacity));
    if (count == 0) return std::nullopt;
    queue_.push_back_batch(std::span{batch}.first(count - 1));
    return batch[count - 1];
  }
  // 空闲时领取搜索名额并尝试窃取，失败后再检查一次全局队列。
  std::optional<std::coroutine_handle<>> steal_task() {
    if (!searching_) searching_ = domain_.start_searching();
    if (searching_) {
      // 每个本地调度器独有的轻量状态，不使用共享随机数锁或 random_device 热路径。
      random_state_ ^= random_state_ << 13;
      random_state_ ^= random_state_ >> 7;
      random_state_ ^= random_state_ << 17;
      if (auto task = domain_.steal_into(worker_id_, queue_, random_state_ % domain_.num_workers()))
        return task;
    }
    return domain_.global_queue().try_pop();
  }
  // 真正执行任务前取消搜索标识，搜索计数不包含正在运行用户代码的线程。
  void before_execute() noexcept {
    if (searching_) { searching_ = false; domain_.stop_searching(); }
  }
  // 所属线程检查本地快速槽和环形队列，不读取其他线程私有快速槽。
  bool has_local_task() const noexcept { return !queue_.empty_local(); }
  // 仅所属线程查询；未领取搜索名额的线程直接休眠，减少空闲扫描争用。
  bool is_searching() const noexcept { return searching_; }
  // 入睡登记后再次检查本地和全局工作，构成防丢失唤醒的关键握手。
  bool has_ready_task() const noexcept { return has_local_task() || !domain_.global_queue().empty(); }
  // 准备进入底层等待；调用者必须在登记之后再次检查 has_ready_task。
  bool prepare_sleep() {
    if (has_local_task()) return false;
    domain_.prepare_sleep(worker_id_, searching_);
    searching_ = false;
    return true;
  }
  // I/O 返回或二次检查发现任务后撤销休眠，恢复与共享计数一致的搜索状态。
  bool finish_sleep() {
    if (has_ready_task()) {
      searching_ = !domain_.cancel_sleep(worker_id_);
      return true;
    }
    if (domain_.still_sleeping(worker_id_)) return false;
    searching_ = true; // 唤醒方已摘除休眠记录并增加搜索计数。
    return true;
  }

private:
  domain_scheduler& domain_;                   // 借用整个 runtime 的共享调度域。
  std::size_t worker_id_;                      // 本地调度器对应的线程编号。
  std::uint32_t global_queue_interval_;        // 全局队列公平性检查间隔，配置保证非零。
  std::uint64_t random_state_;                 // 所属线程私有的窃取起点生成状态。
  domain_scheduler::local_queue_type queue_;   // 本地环形队列与私有快速恢复槽。
  bool searching_{false};                      // 本线程是否已领取共享搜索名额。
};
static_assert(ready_sink<local_scheduler>);
} // namespace faio::runtime::detail
#endif // FAIO_DETAIL_RUNTIME_CORE_SCHEDULER_LOCAL_SCHEDULER_HPP
