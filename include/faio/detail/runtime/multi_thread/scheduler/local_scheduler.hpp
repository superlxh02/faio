/**
 * @file faio/detail/runtime/multi_thread/scheduler/local_scheduler.hpp
 * @brief 本地任务获取、公平性与窃取。
 */
#ifndef FAIO_DETAIL_RUNTIME_MULTI_THREAD_SCHEDULER_LOCAL_SCHEDULER_HPP
#define FAIO_DETAIL_RUNTIME_MULTI_THREAD_SCHEDULER_LOCAL_SCHEDULER_HPP

#include "faio/detail/runtime/multi_thread/scheduler/domain_scheduler.hpp"
#include "faio/detail/runtime/multi_thread/scheduler/local_execution_budget.hpp"
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace faio::runtime::detail {
/**
 * @brief 每个 worker 一个本地调度器：管理本地队列、公平性、批量拉取、窃取与休眠转换。
 * 所属线程操作私有状态，其他线程只能经 domain_scheduler 窃取其环形队列。
 */
template <class W>
class basic_local_scheduler {
  using domain_type = basic_domain_scheduler<W>;

 public:
  /** @brief 借用共享调度域并登记本地队列；底层唤醒端必须存活到本对象注销之后。 */
  basic_local_scheduler(domain_type& domain,
                        std::size_t worker_id,
                        std::uint32_t global_queue_interval,
                        basic_worker_waker_ref<W> waker,
                        std::size_t no_stealing_queue_capacity = LOCAL_QUEUE_CAPACITY)
      : domain_(domain),
        worker_id_(worker_id),
        global_queue_interval_(global_queue_interval),
        random_state_(worker_id + 1),
        private_queue_(no_stealing_queue_capacity) {
    if (global_queue_interval == 0)
      throw std::invalid_argument("全局队列检查间隔必须大于零");
    domain_.register_local(worker_id_, queue_, private_queue_, waker);
  }

  /** @brief 禁止复制构造工作线程本地调度器。 */
  basic_local_scheduler(const basic_local_scheduler&) = delete;

  /** @brief 禁止复制赋值。 */
  basic_local_scheduler& operator=(const basic_local_scheduler&) = delete;

  /** @brief 所有 worker 的事件循环已通过退出屏障后才调用此析构。 */
  ~basic_local_scheduler() { domain_.unregister_local(worker_id_); }

  /** @brief 把本地队列作为线程绑定状态；通用调度入口据此直接内联本地入队。 */
  typename domain_type::local_queue_type& local_state() noexcept { return queue_; }

  /** @brief 接收 I/O/定时器完成的就绪协程，批量通知在 flush_ready 后统一执行。 */
  void enqueue_ready(std::coroutine_handle<> task) {
    queue_.push_back(task, domain_.global_queue());
  }

  /** @brief 一轮完成事件发布完毕后按需唤醒同伴，避免每个 CQE 都执行唤醒判断。 */
  void flush_ready() noexcept {
    if (!searching_ && (!queue_.empty() || !domain_.global_queue().empty()))
      domain_.wake_up_one();
  }

  /**
   * @brief 获取下一任务；周期性检查全局队列，防止连续本地工作饿死外部提交。
   * @param tick 当前调度轮次，用于全局队列公平性检查。
   * @param force_global 是否补做之前到期但被其他执行来源推迟的全局检查。
   * @return 当前线程独占领取的句柄；全部已检查来源为空时返回 std::nullopt。
   */
  std::optional<std::coroutine_handle<>> next_task(std::uint32_t tick, bool force_global = false) {
    return next_task_impl<false>(tick, force_global);
  }

  /** @brief 单驱动线程持锁消费队列，并按自身预算决定是否先检查远端队列。 */
  std::optional<std::coroutine_handle<>> next_owner_task(bool check_global) {
    assert(domain_.num_workers() == 1);
    return next_task_impl<true>(0, check_global);
  }

  /**
   * @brief 空闲时领取搜索名额并尝试窃取，失败后再检查一次全局队列。
   * @return 窃取批次的直接执行项或全局回退项；未取得任务时返回 std::nullopt。
   * @details 只有共享状态机允许的搜索线程扫描同伴队列；随机起点由本线程
   * 私有状态生成。取得任务时不在此恢复协程，实际执行边界再归还搜索名额。
   */
  std::optional<std::coroutine_handle<>> steal_task() {
    last_task_from_fast_ = false;
    if (auto task = private_queue_.try_pop())
      return task;
    if (!searching_)
      searching_ = domain_.start_searching();
    if (searching_) {
      random_state_ ^= random_state_ << 13;
      random_state_ ^= random_state_ >> 7;
      random_state_ ^= random_state_ << 17;
      if (auto task = domain_.steal_into(worker_id_, queue_, random_state_ % domain_.num_workers()))
        return task;
    }
    if (auto task = domain_.global_queue().try_pop())
      return task;
    interrupt_execution_chain();
    return std::nullopt;
  }

  /**
   * @brief 工作线程实际恢复协程时，领取与任务来源对应的预算；作用域退出时保存剩余额度并恢复外层 TLS。
   */
  auto begin_execution() noexcept { return execution_budget_.begin(last_task_from_fast_); }

  /**
   * @brief 普通驱动、因时间预算触发的驱动或休眠均结束连续快速槽执行链；已选中但尚未执行的任务仍从完整预算开始。
   */
  void interrupt_execution_chain() noexcept { execution_budget_.interrupt(); }

  /** @brief 真正执行任务前取消搜索标识，搜索计数不包含正在运行用户代码的线程。 */
  void before_execute() noexcept {
    if (searching_) {
      searching_ = false;
      domain_.stop_searching();
    }
  }

  /** @brief 所属线程检查本地快速槽和环形队列，不读取其他线程私有快速槽。 */
  bool has_local_task() const noexcept { return !queue_.empty_local() || !private_queue_.empty(); }

  /** @brief 仅所属线程查询；未领取搜索名额的线程直接休眠，减少空闲扫描争用。 */
  bool is_searching() const noexcept { return searching_; }

  /** @brief 入睡登记后再次检查本地和全局工作，构成防丢失唤醒的关键握手。 */
  bool has_ready_task() const noexcept {
    return has_local_task() || !domain_.global_queue().empty();
  }

  /** @brief 准备进入底层等待；调用者必须在登记之后再次检查 has_ready_task。 */
  bool prepare_sleep() {
    if (has_local_task())
      return false;
    domain_.prepare_sleep(worker_id_, searching_);
    searching_ = false;
    return true;
  }

  /** @brief I/O 返回或二次检查发现任务后撤销休眠，恢复与共享计数一致的搜索状态。 */
  bool finish_sleep(bool additional_ready = false) {
    if (additional_ready || has_ready_task()) {
      searching_ = !domain_.cancel_sleep(worker_id_);
      flush_ready();

      return true;
    }
    if (domain_.still_sleeping(worker_id_))
      return false;
    searching_ = true;
    return true;
  }

 private:
  friend class current_thread_owner_scheduler;

  template <bool owner_only>
  bool global_check_due(std::uint32_t tick, bool force_global) const noexcept {
    if constexpr (owner_only)
      return force_global;
    else
      return force_global || tick % global_queue_interval_ == 0;
  }

  template <bool owner_only>
  std::optional<std::coroutine_handle<>> next_task_impl(std::uint32_t tick, bool force_global) {
    last_task_from_fast_ = false;
    if (!private_queue_.empty()) {
      if (prefer_private_) {
        if (auto task = private_queue_.try_pop()) {
          private_global_check_pending_ =
              private_global_check_pending_ || global_check_due<owner_only>(tick, force_global);
          prefer_private_ = false;
          return task;
        }
      }
      const bool check_global = force_global || private_global_check_pending_;
      private_global_check_pending_ = false;
      auto task = next_ordinary_task<owner_only>(tick, check_global);
      prefer_private_ = true;
      if (task)
        return task;
      return private_queue_.try_pop();
    }
    const bool check_global = force_global || private_global_check_pending_;
    private_global_check_pending_ = false;
    return next_ordinary_task<owner_only>(tick, check_global);
  }

  /** @brief 领取下一个普通就绪任务。 */
  template <bool owner_only>
  std::optional<std::coroutine_handle<>> next_ordinary_task(std::uint32_t tick, bool force_global) {
    last_task_from_fast_ = false;
    if (global_check_due<owner_only>(tick, force_global)) {
      if (auto task = domain_.global_queue().try_pop())
        return task;
      return next_local_task<owner_only>();
    }
    if (auto task = next_local_task<owner_only>())
      return task;
    std::array<std::coroutine_handle<>, LOCAL_QUEUE_CAPACITY / 2> batch;
    const auto capacity = std::min(queue_.remaining_capacity(), batch.size());
    const auto count = domain_.global_queue().try_pop_batch(std::span{batch}.first(capacity));
    if (count == 0) {
      interrupt_execution_chain();
      return std::nullopt;
    }
    queue_.push_back_batch(std::span{batch}.first(count - 1));
    return batch[count - 1];
  }

  /**
   * @brief 协作预算耗尽时先将快速槽任务降级，避免反复让出的 FIFO 任务独占预算。
   * @details 单驱动线程已经在消费就绪队列，降级后无需唤醒其他线程。
   */
  template <bool owner_only>
  std::optional<std::coroutine_handle<>> next_local_task() {
    if (execution_budget_.exhausted()) {
      if constexpr (owner_only) {
        queue_.demote_fast(domain_.global_queue());
      } else {
        const bool another_ready = !queue_.empty();
        if (queue_.demote_fast(domain_.global_queue())
            && (another_ready || !domain_.global_queue().empty()))
          domain_.wake_up_one();
      }
    }
    auto task = queue_.template try_pop_local<owner_only>();
    last_task_from_fast_ = task && queue_.last_pop_from_fast();
    if (!task)
      interrupt_execution_chain();
    return task;
  }

  local_execution_budget
      execution_budget_; /**< 工作线程私有的连续执行链，不向协程帧添加字段，也不跨线程共享。 */
  bool last_task_from_fast_{
      false};             /**< 当前选中任务的来源；驱动器可仅中断连续执行链，无需修改句柄。 */
  domain_type& domain_;   /**< 借用整个 runtime 的共享调度域。 */
  std::size_t worker_id_; /**< 本地调度器对应的线程编号。 */
  std::uint32_t global_queue_interval_;          /**< 全局队列公平性检查间隔，配置保证非零。 */
  std::uint64_t random_state_;                   /**< 所属线程私有的窃取起点生成状态。 */
  typename domain_type::local_queue_type queue_; /**< 本地环形队列与私有快速恢复槽。 */
  typename domain_type::private_queue_type private_queue_; /**< 多生产者投递，仅本 worker 消费。 */
  bool searching_{false};                                  /**< 本线程是否已领取共享搜索名额。 */
  bool prefer_private_{true};           /**< 不可窃取队列与普通队列同时有任务时交替消费。 */
  bool private_global_check_pending_{}; /**< 私有分支推迟的全局公平检查，下一普通分支补做。 */
};

using local_scheduler = basic_local_scheduler<io_worker_waker>;
static_assert(ready_sink<local_scheduler>);
}  // namespace faio::runtime::detail
#endif
