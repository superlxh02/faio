#pragma once
#if defined(__linux__) && defined(FAIO_HAS_IO_URING) && FAIO_HAS_IO_URING
#include "faio/detail/io/backend_protocol.hpp"
#include "faio/detail/io/backend_selection.hpp"
#include "faio/detail/io/core/resource_state.hpp"
#include "faio/detail/io/operation.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <deque>
#include <liburing.h>
#include <limits>
#include <linux/openat2.h>
#include <linux/stat.h>
#include <memory_resource>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <system_error>
#include <unistd.h>
#include <unordered_map>

namespace faio::io::detail {
/** @brief 原生 Linux Proactor；SQ/CQ、取消确认和零拷贝释放共用稳定代际协议。
 * @details user_data
 * 是不复用的整数完成键；它查找后端拥有的记录，绝不存协程地址。
 *          接受请求后即持有内核引用，只有最终 CQE/NOTIF
 * 才可解除请求与缓冲区租约。 跨线程 submit/cancel 用短锁串行；等待使用 ring fd
 * 原生GETEVENTS；eventfd只有一个POLL_ADD控制SQE，不持锁睡眠。
 */
class uring_backend {
public:
  static constexpr bool native_proactor = true;
  static constexpr bool poll_flushes_submissions =
      true; ///< poll 首先提交 SQ/取消责任，无需域重复 flush。
  static constexpr const char *backend_name = "uring";
  /** @brief 建立私有 ring、控制 eventfd 和只读能力缓存；不注册业务 fd 的
   * readiness。
   * @param capacity 请求的 SQ 容量，最终容量由内核和 CLAMP 约束。
   * @param use_extended_wait 是否使用探测到的 EXT_ARG；false 保留原生 TIMEOUT
   * 兼容路径。
   * @throws std::system_error
   * 任一原生控制资源初始化失败；构造阶段不接受业务请求。
   */
  explicit uring_backend(unsigned capacity = 256,
                         bool use_extended_wait = true) {
    // 两批 SQ 深度预热普通记录/取消 ACK，再为 TIMEOUT 与其 ACK
    // 留两个常见控制槽。
    // 这里不是最大在途上限：域已有操作/资源容量约束真正接受责任，峰值超过预热时可增长。
    // 所有可能抛出分配都发生在创建 ring
    // 以前，构造失败不需要回收尚不存在的内核映射。
    const auto preheated_entries = static_cast<std::size_t>(capacity) * 2 + 2;
    entries_.reserve(
        preheated_entries); // 同时预热 bucket，避免稳定流量中反复 rehash。
    for (std::size_t index = 1; index <= preheated_entries; ++index)
      entries_.try_emplace(index); // 创建与真实记录完全相同的节点大小及对齐。
    entries_.clear(); // 节点回到私有池；next_key_ 不变，预热键从未提交给内核。
    io_uring_params params{}; // 所有保留位清零，避免内核拒绝未知 flags。
    params.flags = IORING_SETUP_CLAMP; // 允许内核将 SQ 容量约束到可支持上限。
    const int result = ::io_uring_queue_init_params(
        capacity, &ring_, &params); // 一次创建共享 SQ/CQ 映射。
    if (result < 0)
      throw std::system_error(-result, std::generic_category(),
                              "io_uring_queue_init: 原生后端初始化失败；显式 "
                              "IO_EPOLL 可使用就绪后端");
    initialized_ = true; // 从此任何构造失败路径都必须销毁 ring。
    // 等待扩展按实际 feature 位选择；后续 poll 不再执行版本或能力探测。
    extended_wait_ =
        use_extended_wait && (params.features & IORING_FEAT_EXT_ARG);
    wake_fd_ = ::eventfd(
        0,
        EFD_CLOEXEC | EFD_NONBLOCK); // 跨线程控制唤醒不借用任何 coroutine 帧。
    if (wake_fd_ < 0) {
      const int error = errno; // 先保存 eventfd 的错误，回收调用不得覆盖它。
      ::io_uring_queue_exit(
          &ring_); // 构造失败时回收已经成功建立的 SQ/CQ 映射。
      initialized_ = false;
      throw std::system_error(error, std::generic_category(), "uring eventfd");
    }
    auto *probe = ::io_uring_get_probe_ring(
        &ring_); // 初始化时一次探测，普通 IO 不查版本/内核 opcode。
    if (!probe) {
      ::close(wake_fd_);
      ::io_uring_queue_exit(&ring_);
      initialized_ = false;
      throw std::system_error(ENOMEM, std::generic_category(),
                              "io_uring opcode probe");
    }
    for (unsigned i = 0; i < supported_.size(); ++i)
      supported_[i] = ::io_uring_opcode_supported(
          probe, static_cast<int>(i)); // 缓存内核实际支持能力。
    ::io_uring_free_probe(probe);      // probe 临时资源完成使用即释放。
    utsname
        info{}; // opcode probe 不描述子命令/flag 的语义，需要一次版本边界判定。
    if (::uname(&info) == 0) {
      try {
        const auto version = parse_kernel_version(info.release);
        socket_commands_ =
            version.major > 6 || (version.major == 6 && version.minor >= 7);
        accept_nowait_ =
            version.major > 6 || (version.major == 6 && version.minor >= 10);
      } catch (const std::invalid_argument &) {
        socket_commands_ =
            false; // 版本不可判断时选择等价短 syscall，不泄漏已创建 ring。
      }
    }
  }
  ~uring_backend() {
    // domain 必须先 drain；queue_exit 只负责已经没有 payload 借用的内核对象。
    if (initialized_)
      ::io_uring_queue_exit(&ring_);
    if (wake_fd_ >= 0)
      ::close(wake_fd_);
  }
  uring_backend(const uring_backend &) = delete;
  const char *name() const noexcept { return backend_name; }
  /// @brief 原生普通 fd 请求直接进入 SQE，无需额外的长期 readiness 注册。
  int attach(int, std::uint64_t) noexcept { return 0; }
  /// @brief 本后端未安装 fd readiness；原生操作的引用通过最终 CQE 独立排空。
  void detach(int) noexcept {}
  /// @brief 查询构造时固定的 opcode/flag 能力，不调用业务 syscall
  /// 或启动服务线程。
  bool supports(std::uint32_t kind) const noexcept {
    const auto operation = static_cast<operation_kind>(kind);
    if (operation == operation_kind::accept_nowait && !accept_nowait_)
      return false; // flag 语义在接受前检查，避免旧内核暗中等待连接。
    if ((operation == operation_kind::getsockopt ||
         operation == operation_kind::setsockopt ||
         operation == operation_kind::socket_inq ||
         operation == operation_kind::socket_outq) &&
        !socket_commands_)
      return false; // URING_CMD 的设备能力不代表 socket
                    // 扩展；6.7以前在接受前选短 syscall。
    const int opcode = opcode_for(static_cast<operation_kind>(kind));
    return opcode >= 0 && static_cast<unsigned>(opcode) < supported_.size() &&
           supported_[opcode];
  }
  backend_statistics statistics() const noexcept {
    std::lock_guard lock(mutex_);
    return statistics_;
  }
  /** @brief SQ 满只返回 would_queue；尚未接管时调用者可安全保存在有界域队列。
   * @details 一旦 accepted，即使 submit 尚未进入内核也不可立即释放 request。
   */
  backend_submit_result try_submit(backend_operation operation) noexcept {
    std::lock_guard lock(
        mutex_); // SQ 写入、key 表及控制取消共用短锁，driver 等待不持此锁。
    auto &request = *static_cast<io_request *>(
        operation.request); // 域槽拥有，接受前已固定参数地址。
    if (!supports(static_cast<std::uint32_t>(request.kind)))
      return {backend_submit_status::rejected, EOPNOTSUPP};
    if (!pending_cancels_.empty())
      issue_cancels(); // 有真实取消责任才进入控制准备，仍先于业务请求取得新 SQ
                       // 容量。
    auto *sqe =
        ::io_uring_get_sqe(&ring_); // 仅保留 SQ 槽，此时未向内核提交 payload。
    if (!sqe)
      return {backend_submit_status::would_queue, 0};
    const auto key = ++next_key_;
    if (!key || key >= wake_key_)
      std::terminate(); // 完成键不循环复用，避免任何迟到 CQE 的 ABA。
    try {
      auto [it, inserted] = entries_.try_emplace(
          key); // 节点地址稳定，open_how 不因 rehash 移动。
      auto &entry =
          it->second; // 后端独立拥有原 CQE 记录，与控制 ACK 记录分开。
      entry.token =
          operation.token;      // 保留完整代际，核心 lookup 仍须检查该代际。
      entry.request = &request; // 最后内核事件以前不允许域回收这个槽。
      entry.sigpipe_sensitive = request.resource &&
                                !request.resource->regular_file &&
                                (request.kind == operation_kind::write ||
                                 request.kind == operation_kind::writev);
      prepare(*sqe, request, entry); // 只写 SQE，尚未发生 syscall 副作用。
      request.native_completion_key =
          key; // 已稳定请求直接保存key，不再建第二张hash。
      if (entry.sigpipe_sensitive)
        sqe->flags |= IOSQE_ASYNC; // 管道/流 WRITE 交 io-wq；其内核 worker 屏蔽
                                   // SIGPIPE，保留 EPIPE CQE。
      if (entry.sigpipe_sensitive)
        ++sigpipe_operations_;
    } catch (...) {
      // get_sqe 已保留 SQ 槽；NOP 无 payload 引用，不能留下未初始化 opcode。
      ::io_uring_prep_nop(sqe);
      ::io_uring_sqe_set_data64(sqe, 0);
      entries_.erase(key);
      request.native_completion_key = 0;
      return {backend_submit_status::rejected, ENOMEM};
    }
    ::io_uring_sqe_set_data64(sqe,
                              key); // CQE 只携带整数，不携带 awaiter 地址。
    ++statistics_.native_submitted; // 业务 SQE 接受；native_flushed
                                    // 记录内核接管的 SQE 数。
    return {backend_submit_status::accepted, 0};
  }
  /** @brief 短 SQ 锁内推进一次提交；返回的 submitted 是内核实际接受数量。
   * @details 信号保护只覆盖可能执行管道/流 WRITE 的 enter/task_work 窗口。
   */
  backend_flush_result flush() noexcept {
    std::lock_guard lock(mutex_);
    sigpipe_scope protected_signals{sigpipe_operations_ != 0};
    return flush_locked();
  }
  /** @brief 取消确认不是业务完成；原请求 CQE 到达以前一直保留内核租约。 */
  void request_cancel(backend_operation operation) noexcept {
    // 取消队列与 SQ 准备共用锁；原请求和 ACK 仍分别持有自己的完成记录。
    std::lock_guard lock(mutex_);
    if (!operation.request)
      return;
    // 调用方域租约固定 typed request，不从 awaiter 地址或数值 fd 推断取消身份。
    const auto &request = *static_cast<const io_request *>(operation.request);
    // 稳定请求保存唯一原生键，随后完整 token 校验拒绝迟到代际。
    const auto original = entries_.find(request.native_completion_key);
    if (original == entries_.end() ||
        original->second.token != operation.token ||
        original->second.cancel_requested)
      return; // 验证完整代际，不用高位tag也不依赖awaiter地址。
    // 首次取消取得一次控制责任；重复回调不能再准备相同取消。
    original->second.cancel_requested = true;
    try {
      // 尚无 SQ 容量时保留原完成键，稍后 flush 优先准备原生 ASYNC_CANCEL。
      pending_cancels_.push_back(original->first);
    } catch (...) {
      std::terminate();
    } // 接受后的取消责任不能因 OOM 静默丢失。
    // 有 SQ 空位立即准备 ACK 请求，真正进入内核仍由提交路径负责。
    issue_cancels();
    // 打断 GETEVENTS，使 driver 看到新取消责任；此通知不代替原 CQE。
    wake();
  }
  /** @brief 唯一CQ消费者；等待直接使用原生GETEVENTS，不用业务fd就绪模拟。
   * @details eventfd仅有一个原生POLL_ADD控制唤醒SQE；remote submit/cancel先prep
   * 再写该eventfd。等待时不持SQ锁，enter的to_submit=0不读取remote写入的SQ。
   * 新内核EXT_ARG传栈上timeout；5.10无EXT_ARG时使用拥有型TIMEOUT SQE。
   */
  int poll(std::span<backend_event> output,
           std::optional<int> timeout) noexcept {
    // SQ 锁内快照决定等待窗口是否需要防 SIGPIPE，不持锁等待远程任务。
    bool protect_wait;
    int count;
    {
      // 唯一 CQ 消费者仍用 SQ 短锁保护记录表与并发 prep/cancel 的交接。
      std::lock_guard lock(mutex_);
      // 尚在途的管道 WRITE 可能在 enter/task_work 执行，网络 SEND 不需此保护。
      protect_wait = sigpipe_operations_ != 0;
      sigpipe_scope signals{protect_wait};
      // 仅在 poll 入口领取先前通知；普通 false 路径只 load，不执行多余原子
      // RMW。 与 wake 的 seq_cst 全序划分入口前后，harvest
      // 排空字节绝不撤销新通知责任。
      const bool pending_notice = wake_pending_.load(std::memory_order_seq_cst);
#if defined(FAIO_URING_CONTROL_TESTS) && FAIO_URING_CONTROL_TESTS
      control_test_step(control_test_point::poll_after_load);
#endif
      const bool noticed =
          pending_notice &&
          wake_pending_.exchange(false, std::memory_order_seq_cst);
#if defined(FAIO_URING_CONTROL_TESTS) && FAIO_URING_CONTROL_TESTS
      control_test_step(control_test_point::poll_after_consume);
#endif
      // 只记录本轮 wake/TIMEOUT 消费，不能把上轮控制推进当作本轮完成。
      control_progress_ = false;
      // 先把已接受 SQE 与控制取消交给内核，再读取其真正的 CQE。
      const auto flushed = flush_locked();
      if (flushed.error)
        return -flushed.error;
      // 有界消费现成 CQE；普通结果、ACK、NOTIF 由同一个解码入口区分。
      count = harvest(output);
      // 先消费真实 CQE，再把尚未提交的
      // SQ/取消责任交回宿主重试；控制不恢复业务协程。 pending 时原生控制
      // POLL_ADD 可能尚未入内核，绝不能依赖它结束无限 GETEVENTS。 noticed
      // 无条件禁止本调用睡眠，连 poll(0) 也领取同一通知；宿主随后重查真实工作。
      if (count || control_progress_ || noticed || flushed.pending ||
          (timeout && !*timeout))
        return count;
      if (timeout && !extended_wait_) {
        // 老内核的liburing wait_cqe_timeout会修改SQ，不能与remote prep并发。
        const int prepared = arm_wait_timer(*timeout);
        if (prepared < 0)
          return 0; // SQ/内存背压尚未接受timeout，不进行无限等待也不伪造永久错误。
        // TIMEOUT 的拥有型参数已经稳定，先提交它才能安全进入无限 GETEVENTS。
        const auto timer_flushed = flush_locked();
        if (timer_flushed.error)
          return -timer_flushed.error;
        // 已准备的 TIMEOUT 参数仍由原 entry/timer_key 拥有，下一轮先重试同一个
        // SQE。 只有本地 SQ/控制责任全部成功提交后才进入等待；不能把 prep 当作
        // kernel accept。
        if (timer_flushed.pending)
          return harvest(
              output); // 有界消费同期真实完成；不伪造超时或重复创建 timer。
      }
    }
    sigpipe_scope protected_wait{protect_wait}; // task_work也可执行流WRITE。
    int waited;
#if defined(FAIO_URING_CONTROL_TESTS) && FAIO_URING_CONTROL_TESTS
    control_test_step(control_test_point::poll_before_wait);
#endif
    do {
#if defined(FAIO_URING_CONTROL_TESTS) && FAIO_URING_CONTROL_TESTS
      // 只在专用单 TU
      // 契约目标存在；错误进入等待也能有限失败，避免测试自身永久挂起。
      if (control_test_wait_)
        waited = control_test_wait_(control_test_context_);
      else
#endif
          if (timeout && extended_wait_) {
        // EXT_ARG 不写 SQ；同步 enter 期间栈上 timespec 地址始终有效。
        __kernel_timespec duration{
            *timeout / 1000, static_cast<long long>(*timeout % 1000) * 1000000};
        // 清零保留字段，只向内核提供超时指针，不借用协程帧或远程 SQ。
        io_uring_getevents_arg argument{};
        // UAPI 使用整数地址字段，明确保持本进程指针的完整宽度。
        argument.ts = reinterpret_cast<std::uintptr_t>(&duration);
        // to_submit=0：此等待不读取其他线程正在准备的
        // SQ，最少等待一个原生完成。
        waited = ::io_uring_enter2(
            ring_.ring_fd, 0, 1, IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG,
            reinterpret_cast<sigset_t *>(&argument),
            sizeof(argument)); // 兼容旧liburing的sigset_t*参数声明。
      } else {
        // 无相对 EXT_ARG 时由先前提交的 TIMEOUT 或控制 wake 负责结束原生等待。
        waited = ::io_uring_enter(ring_.ring_fd, 0, 1, IORING_ENTER_GETEVENTS,
                                  nullptr);
      }
      // 信号中断只重试控制等待，不能生成业务完成或提前释放借用范围。
    } while (waited == -EINTR);
    // 原生等待已经结束，现在才重新取得 SQ/记录锁，与远程准备结果串行合并。
    std::lock_guard lock(mutex_);
    sigpipe_scope signals{sigpipe_operations_ != 0};
    count =
        harvest(output); // CQE res/flags才决定业务结果，enter返回不是IO完成。
    cancel_wait_timer(); // 业务完成/控制wake提前到达时，原生取消旧TIMEOUT。
    const auto flushed =
        flush_locked(); // 重新arm控制wake并提交已准备的remote请求。
    if (flushed.error)
      return -flushed.error;
    // 超时与暂态内存/SQ 背压不冒充永久驱动故障，其余错误沿域故障协议处理。
    if (waited < 0 && waited != -ETIME && waited != -EAGAIN &&
        waited != -ENOMEM)
      return waited;
    return count;
  }
  /** @brief 同一个入口 epoch 合并控制通知；业务与取消责任仍保存在稳定
   * SQ/记录中。
   * @details 第一位 false→true 发布者负责 eventfd 写入；之后的通知由同一个
   * latch 覆盖。 poll 入口消费 latch 后必须立即返回宿主，即使旧 wake CQE
   * 把新计数一起排空。 完成/取消结果仍只来自原生 CQE，没有业务 readiness
   * 或辅助线程。
   */
  void wake() const noexcept {
    const bool already_pending =
        wake_pending_.exchange(true, std::memory_order_seq_cst);
#if defined(FAIO_URING_CONTROL_TESTS) && FAIO_URING_CONTROL_TESTS
    control_test_step(control_test_point::wake_after_exchange);
#endif
    if (already_pending)
      return; // 先前发布者或当前 poll 已承担这一 epoch 的通知责任。
    const std::uint64_t one = 1;
    ssize_t written;
#if defined(FAIO_URING_CONTROL_TESTS) && FAIO_URING_CONTROL_TESTS
    control_test_step(control_test_point::wake_before_write);
#endif
    do {
#if defined(FAIO_URING_CONTROL_TESTS) && FAIO_URING_CONTROL_TESTS
      written = control_test_write_
                    ? control_test_write_(wake_fd_, &one, sizeof(one),
                                          control_test_context_)
                    : ::write(wake_fd_, &one, sizeof(one));
#else
      written = ::write(wake_fd_, &one,
                        sizeof(one)); // 正常构建仍是原生控制 syscall。
#endif
    } while (written < 0 &&
             errno == EINTR); // 信号中断不能丢掉本 epoch 唯一写入责任。
    (void)written; // EAGAIN 表示 eventfd 已饱和可读；稳定 fd 的生命周期遵循原
                   // owning 合同。
  }
  /// @brief 停机只打断等待；已接受原生操作仍须由域取消并消费最后内核事件。
  void begin_shutdown() noexcept { wake(); }
  /** @brief 私有 ring 的永久 enter 故障不能以 close(fd) 伪装已解除内核引用。
   * @details EINTR/EAGAIN/ENOMEM
   * 已归为可重试背压；其他永久驱动错误意味着完成协议损坏。 Linux close ring
   * 可异步清理，返回 false 要求宿主 fail-fast，绝不提前恢复借用者。
   */
  bool begin_failure(int) noexcept { return false; }
  /** @brief 查询原生业务/ACK/TIMEOUT 与待提交 SQ 是否都已排空。
   * @details 唯一常驻控制 wake 不持有业务 payload；ring
   * 析构仍由域停机纪律负责。
   */
  bool quiescent() const noexcept {
    std::lock_guard lock(mutex_);
    return entries_.empty() && pending_cancels_.empty() &&
           ::io_uring_sq_ready(&ring_) == 0;
  }

private:
  /** @brief io_uring 在提交线程内执行管道 WRITE 时仍可发 SIGPIPE。
   * @details 只修改当前线程掩码，在 enter/可触发 task_work 的 CQ
   * 读取窗口内屏蔽； 消费本窗口新产生的 pending SIGPIPE，保留调用前 pending
   * 信号和原掩码。 网络 SEND 使用 MSG_NOSIGNAL，不增加线程信号
   * syscall；文件同样无需此保护。
   */
  class sigpipe_scope {
  public:
    explicit sigpipe_scope(bool enabled) noexcept : enabled_(enabled) {
      if (!enabled_)
        return;
      // 只构造 SIGPIPE 的线程局部屏蔽集合，其他信号保持原有语义。
      ::sigemptyset(&signal_);
      // 流 WRITE 的 EPIPE 应通过 CQE 返回，不能让进程先因默认信号动作退出。
      ::sigaddset(&signal_, SIGPIPE);
      // 保存原掩码，无法建立信号边界时不得继续执行敏感请求。
      if (::pthread_sigmask(SIG_BLOCK, &signal_, &previous_) != 0)
        std::terminate();
      sigset_t pending{};
      (void)::sigpending(&pending);
      // 记录进入前已有信号，退出时只消费本窗口新产生的 SIGPIPE。
      already_pending_ = ::sigismember(&pending, SIGPIPE) == 1;
    }
    ~sigpipe_scope() {
      if (!enabled_)
        return;
      if (!already_pending_) {
        sigset_t pending{};
        (void)::sigpending(&pending);
        if (::sigismember(&pending, SIGPIPE) == 1) {
          // 零时长消费本线程的新 pending 信号，不在协程 worker 上等待。
          timespec immediately{};
          (void)::sigtimedwait(&signal_, nullptr, &immediately);
        }
      }
      // 退出原生窗口恢复调用方掩码，不把屏蔽状态传播到后续用户代码。
      (void)::pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
    }

  private:
    sigset_t signal_{}, previous_{};
    bool enabled_{}, already_pending_{};
  };
  struct entry {
    std::uint64_t
        token{}; ///< 域操作完整代际；ACK 同样保留它但不据此释放原请求。
    io_request *request{}; ///< 指向稳定域槽，原请求最后 CQE 以前由域租约固定。
    open_how how{};        ///< openat2 的扩展参数由后端记录拥有，unordered_map
                           ///< 不移动节点地址。
    __kernel_timespec timeout{}; ///< 5.10原生TIMEOUT参数稳定到最终CQE。
    bool ack{}, cancel_requested{}, awaiting_notification{},
        sigpipe_sensitive{}, control_timeout{};
  };
  /// @brief 只作 operation 到真实内核 opcode 的映射；缺项在接受前明确拒绝。
  static int opcode_for(operation_kind kind) noexcept {
    switch (kind) {
    case operation_kind::recv:
      return IORING_OP_RECV;
    case operation_kind::send:
      return IORING_OP_SEND;
    case operation_kind::recvfrom:
    case operation_kind::recvmsg:
      return IORING_OP_RECVMSG;
    case operation_kind::sendto:
    case operation_kind::sendmsg:
      return IORING_OP_SENDMSG;
    case operation_kind::send_zc:
      return IORING_OP_SEND_ZC;
    case operation_kind::sendmsg_zc:
      return IORING_OP_SENDMSG_ZC;
    case operation_kind::connect:
      return IORING_OP_CONNECT;
    case operation_kind::accept:
    case operation_kind::accept_nowait:
      return IORING_OP_ACCEPT;
    case operation_kind::read:
      return IORING_OP_READ;
    case operation_kind::write:
      return IORING_OP_WRITE;
    case operation_kind::readv:
      return IORING_OP_READV;
    case operation_kind::writev:
      return IORING_OP_WRITEV;
    case operation_kind::open:
      return IORING_OP_OPENAT;
    case operation_kind::open2:
      return IORING_OP_OPENAT2;
    case operation_kind::fsync:
      return IORING_OP_FSYNC;
    case operation_kind::close:
      return IORING_OP_CLOSE;
    case operation_kind::shutdown:
      return IORING_OP_SHUTDOWN;
    case operation_kind::socket:
      return IORING_OP_SOCKET;
    case operation_kind::ready:
      return IORING_OP_POLL_ADD;
    case operation_kind::getsockopt:
    case operation_kind::setsockopt:
    case operation_kind::socket_inq:
    case operation_kind::socket_outq:
      return IORING_OP_URING_CMD;
    case operation_kind::statx:
      return IORING_OP_STATX;
    case operation_kind::mkdirat:
      return IORING_OP_MKDIRAT;
    case operation_kind::unlinkat:
      return IORING_OP_UNLINKAT;
    case operation_kind::renameat:
      return IORING_OP_RENAMEAT;
    case operation_kind::linkat:
      return IORING_OP_LINKAT;
    case operation_kind::symlinkat:
      return IORING_OP_SYMLINKAT;
    case operation_kind::ftruncate:
      return IORING_OP_FTRUNCATE;
    default:
      return -1;
    }
  }
  /** @brief 稳定 typed request 直接准备原生 SQE；此函数不
   * submit、不挂起或恢复协程。
   * @param sqe 已保留但尚未提交的 SQ 槽，调用方持有 SQ 锁。
   * @param r 域拥有型请求，路径、地址及 iovec 描述在最后 CQE 前保持稳定。
   * @param owner 后端节点拥有 open_how 等扩展参数；借用 payload 仍由外层 lease
   * 固定。
   * @details 数据 IO 使用对应原生 opcode；只有用户显式 ready 才准备 POLL_ADD。
   */
  static void prepare(io_uring_sqe &sqe, io_request &r, entry &owner) {
    // 标量 IO 的 SQE 长度为 unsigned；超过范围以合法短 IO
    // 返回，由上层处理余量。
    const auto length =
        static_cast<unsigned>(std::min<std::size_t>(r.length, UINT_MAX));
    switch (r.kind) {
    case operation_kind::recv:
      // 接收直接把借用输出缓冲交内核，字节数/EOF/错误仅由真实 CQE 决定。
      ::io_uring_prep_recv(&sqe, r.fd, r.buffer, length, r.flags);
      break;
    case operation_kind::send:
      // 普通发送不先试同步 send；MSG_NOSIGNAL 保持 EPIPE 的错误完成语义。
      ::io_uring_prep_send(&sqe, r.fd, r.const_buffer, length,
                           r.flags | MSG_NOSIGNAL);
      break;
    case operation_kind::send_zc:
      // 零拷贝发送仍借用原 payload；初始 MORE 结果后继续等待 NOTIF 才可归还。
      ::io_uring_prep_send_zc(&sqe, r.fd, r.const_buffer, length,
                              r.flags | MSG_NOSIGNAL,
                              static_cast<unsigned>(r.argument));
      break;
    case operation_kind::recvfrom:
      // recvfrom 经原生 RECVMSG 表达；单元素 iovec 由稳定请求自身拥有。
      r.scalar_vector = {r.buffer, r.length};
      r.message = {};
      // 输出地址/长度由等待帧租约固定；只有完成成功后才回填长度。
      r.message.msg_name = r.output_address;
      // 初始长度是调用方地址容量，内核写出的实际长度待 CQE 后读取。
      r.message.msg_namelen =
          r.output_address_length ? *r.output_address_length : 0;
      r.message.msg_iov = &r.scalar_vector;
      r.message.msg_iovlen = 1;
      // prep 只保存稳定 msghdr 地址，实际接收仍随 SQE 提交执行。
      ::io_uring_prep_recvmsg(&sqe, r.fd, &r.message, r.flags);
      break;
    case operation_kind::recvmsg:
      // 先复制调用方 msghdr 元数据，避免把可移动描述对象本身提交给内核。
      r.message = *r.output_message;
      // 描述符数组拥有型复制；它指向的数据与控制缓冲依然遵守借用生命周期。
      if (r.message.msg_iovlen)
        r.vectors.assign(r.message.msg_iov,
                         r.message.msg_iov + r.message.msg_iovlen);
      // 接收描述符已经拥有型复制，内核只引用稳定数组地址。
      r.message.msg_iov = r.vectors.data();
      ::io_uring_prep_recvmsg(&sqe, r.fd, &r.message, r.flags);
      break;
    case operation_kind::sendto:
      // iovec 历史接口使用 void*；只适配描述类型，不授予内核写入发送 payload
      // 的语义。
      r.scalar_vector = {const_cast<void *>(r.const_buffer), r.length};
      r.message = {};
      // 发送目标地址已复制到拥有型请求，提交不会借用调用者的 sockaddr
      // 临时变量。
      r.message.msg_name = &r.address;
      // 保留目标地址族对应的真实长度，避免把存储容量当作地址长度。
      r.message.msg_namelen = r.address_length;
      r.message.msg_iov = &r.scalar_vector;
      r.message.msg_iovlen = 1;
      // 一条 SENDMSG SQE 同时提交目标地址与数据描述，不拆成 syscall/线程辅助。
      ::io_uring_prep_sendmsg(&sqe, r.fd, &r.message, r.flags | MSG_NOSIGNAL);
      break;
    // 普通向量发送与零拷贝向量发送都复用稳定拥有型 iovec 描述。
    case operation_kind::sendmsg:
      r.message.msg_iov = r.vectors.data();
      ::io_uring_prep_sendmsg(&sqe, r.fd, &r.message, r.flags | MSG_NOSIGNAL);
      break;
    case operation_kind::sendmsg_zc:
      r.message.msg_iov = r.vectors.data();
      // SENDMSG_ZC 的释放通知沿 NOTIF 管线消费，发送结果不等于缓冲已释放。
      ::io_uring_prep_sendmsg_zc(&sqe, r.fd, &r.message,
                                 r.flags | MSG_NOSIGNAL);
      break;
    case operation_kind::connect:
      // CONNECT 直接借用槽内目标地址，连接成功/失败通过 CQE 返回。
      ::io_uring_prep_connect(&sqe, r.fd,
                              reinterpret_cast<sockaddr *>(&r.address),
                              r.address_length);
      break;
    case operation_kind::accept:
    case operation_kind::accept_nowait:
      // 创建的 fd 原子带 NONBLOCK/CLOEXEC；取消获胜仍由统一管线接管成功 fd
      // 的关闭。
      ::io_uring_prep_accept(&sqe, r.fd, r.output_address,
                             r.output_address_length,
                             r.flags | SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (r.kind == operation_kind::accept_nowait)
        sqe.ioprio |=
            IORING_ACCEPT_DONTWAIT; // 业务fd仍直接 ACCEPT，不走poll重试。
      break;
    case operation_kind::read:
      // 非普通文件使用 -1 的流式偏移；普通文件保留定位 offset，不额外执行
      // lseek。
      ::io_uring_prep_read(&sqe, r.fd, r.buffer, length,
                           r.resource && !r.resource->regular_file ? UINT64_MAX
                                                                   : r.offset);
      break;
    case operation_kind::write:
      // WRITE 借用原数据；管道 SIGPIPE 的原生保护由提交窗口与 IOSQE_ASYNC
      // 负责。
      ::io_uring_prep_write(&sqe, r.fd, r.const_buffer, length,
                            r.resource && !r.resource->regular_file ? UINT64_MAX
                                                                    : r.offset);
      break;
    case operation_kind::readv:
      // 向量数量已在上层校验；数组由 r.vectors 拥有，flags 保留原 RWF 语义。
      ::io_uring_prep_readv2(
          &sqe, r.fd, r.vectors.data(), static_cast<unsigned>(r.vectors.size()),
          r.resource && !r.resource->regular_file ? UINT64_MAX : r.offset,
          r.flags);
      break;
    case operation_kind::writev:
      // 定位/流式写偏移直接交内核，允许真实短写，不在此合并或复制 payload。
      ::io_uring_prep_writev2(
          &sqe, r.fd, r.vectors.data(), static_cast<unsigned>(r.vectors.size()),
          r.resource && !r.resource->regular_file ? UINT64_MAX : r.offset,
          r.flags);
      break;
    case operation_kind::open:
      // 路径字符串由稳定请求拥有，dirfd/mode 与 OPENAT 原生参数一一对应。
      ::io_uring_prep_openat(&sqe, r.fd, r.path.c_str(), r.flags | O_CLOEXEC,
                             static_cast<mode_t>(r.argument));
      break;
    case operation_kind::open2:
      // open_how 是后端稳定节点成员，rehash 不移动它，CQE 前不能释放节点。
      owner.how = {r.extension_flags | O_CLOEXEC, r.extension_mode,
                   r.extension_resolve};
      ::io_uring_prep_openat2(&sqe, r.fd, r.path.c_str(), &owner.how);
      break;
    case operation_kind::fsync:
      // 持久化请求直接 FSYNC/DATASYNC flag；不在用户线程同步刷盘。
      ::io_uring_prep_fsync(&sqe, r.fd, r.flags);
      break;
    case operation_kind::close:
      // CLOSE 拥有唯一 fd 关闭责任；接受后即使取消也必须消费真实完成。
      ::io_uring_prep_close(&sqe, r.fd);
      break;
    case operation_kind::shutdown:
      // read/write/both 方向直接交原生 SHUTDOWN，半边租约在域层串行排空。
      ::io_uring_prep_shutdown(&sqe, r.fd, r.argument);
      break;
    case operation_kind::socket:
      // domain/type/protocol 直接创建内核 socket，成功 fd
      // 同样遵守取消清理责任。
      ::io_uring_prep_socket(&sqe, r.argument,
                             r.argument2 | SOCK_NONBLOCK | SOCK_CLOEXEC,
                             r.argument3, r.flags);
      break;
    case operation_kind::ready: {
      // 仅显式 ready API 观察状态；错误/全关闭始终返回，再叠加调用方方向。
      unsigned flags = POLLERR | POLLHUP;
      if (r.argument & 1)
        // 读观察只要求 POLLIN，不把写半关闭误当作可读业务结果。
        flags |= POLLIN;
      if (r.argument & 2)
        // 写观察只要求 POLLOUT，关闭信息的方向转换由 CQE 消费后统一处理。
        flags |= POLLOUT;
      // 这是用户请求的原生 POLL_ADD，普通 RECV/SEND/READ/WRITE 不经过此路径。
      ::io_uring_prep_poll_add(&sqe, r.fd, flags);
      break;
    }
    case operation_kind::getsockopt:
      // socket URING_CMD 子命令已通过能力缓存判断，输出容量和 option
      // 参数直接映射。
      ::io_uring_prep_cmd_sock(&sqe, SOCKET_URING_OP_GETSOCKOPT, r.fd,
                               r.argument, r.argument2, r.buffer,
                               static_cast<int>(r.length));
      break;
    case operation_kind::setsockopt:
      // SETSOCKOPT 仅借用输入 option 字节，完成以前外层不得销毁或修改它。
      ::io_uring_prep_cmd_sock(
          &sqe, SOCKET_URING_OP_SETSOCKOPT, r.fd, r.argument, r.argument2,
          const_cast<void *>(r.const_buffer), static_cast<int>(r.length));
      break;
    case operation_kind::socket_inq:
      // 队列查询把原生整数结果放在 CQE，不为查询创建 blocking job。
      ::io_uring_prep_cmd_sock(&sqe, SOCKET_URING_OP_SIOCINQ, r.fd, 0, 0,
                               nullptr, 0);
      break;
    case operation_kind::socket_outq:
      // 输出队列查询与其他原生操作共用稳定 token 与取消完成管线。
      ::io_uring_prep_cmd_sock(&sqe, SOCKET_URING_OP_SIOCOUTQ, r.fd, 0, 0,
                               nullptr, 0);
      break;
    case operation_kind::statx:
      // path与输出statx存储由稳定请求/外层lease保持到真实CQE。
      ::io_uring_prep_statx(&sqe, r.fd, r.path.c_str(), r.flags,
                            static_cast<unsigned>(r.argument),
                            static_cast<struct statx *>(r.buffer));
      break;
    case operation_kind::mkdirat:
      // mode直接映射原生参数，不在应用线程代做mkdir。
      ::io_uring_prep_mkdirat(&sqe, r.fd, r.path.c_str(),
                              static_cast<mode_t>(r.argument));
      break;
    case operation_kind::unlinkat:
      // AT_REMOVEDIR与普通unlink共用UNLINKAT，保留原生flags。
      ::io_uring_prep_unlinkat(&sqe, r.fd, r.path.c_str(), r.flags);
      break;
    case operation_kind::renameat:
      // 两个dirfd与两个拥有型路径直接映射同一RENAMEAT请求。
      ::io_uring_prep_renameat(&sqe, r.fd, r.path.c_str(), r.argument2,
                               r.path2.c_str(), static_cast<unsigned>(r.flags));
      break;
    case operation_kind::linkat:
      // 不将hard-link拆成元数据操作，直接由内核处理竞态与错误。
      ::io_uring_prep_linkat(&sqe, r.fd, r.path.c_str(), r.argument2,
                             r.path2.c_str(), r.flags);
      break;
    case operation_kind::symlinkat:
      // 第一路径是目标字节，第二路径是新链接名，顺序不可颠倒。
      ::io_uring_prep_symlinkat(&sqe, r.path.c_str(), r.fd, r.path2.c_str());
      break;
    case operation_kind::ftruncate:
      // 有效长度在File层检查，原生请求直接带64位新长度。
      ::io_uring_prep_ftruncate(&sqe, r.fd, static_cast<loff_t>(r.offset));
      break;
    default:
      // 不支持的 kind 已在 try_submit 拒绝；防御分支初始化 SQ 槽且不借用
      // payload。
      ::io_uring_prep_nop(&sqe);
      break;
    }
  }
  /** @brief SQ锁内准备唯一的原生控制唤醒；不是业务socket/file readiness。 */
  bool arm_wake() noexcept {
    // 已有一个控制 POLL_ADD 时不再准备第二个，eventfd 计数会合并远程通知。
    if (wake_armed_)
      return false;
    // 只使用当前 SQ 空位；没有空间时由本轮 submit 后的补 arm 继续承担责任。
    auto *sqe = ::io_uring_get_sqe(&ring_);
    if (!sqe)
      return false;
    // 控制 fd 的原生 POLL_ADD 与业务 fd 完全隔离，且没有借用业务 buffer。
    ::io_uring_prep_poll_add(sqe, wake_fd_, POLLIN);
    // 保留控制键不能与任何普通完成键复用或碰撞。
    ::io_uring_sqe_set_data64(sqe, wake_key_);
    // 标记已准备；只有真实 wake CQE 消费后才能再次 arm。
    wake_armed_ = true;
    return true;
  }
  /** @brief 一次普通submit；SQ曾满时再补提交一个控制wake，不阻塞协程worker。 */
  backend_flush_result flush_locked() noexcept {
    // 空取消队列不调用较大的控制准备函数，免去普通 IO 的寄存器保存/栈帧开销。
    // 真正 pending 时仍先准备 ASYNC_CANCEL，保持取消 ACK 优先与 SQ 背压责任。
    if (!pending_cancels_.empty())
      issue_cancels();
    // 尝试保留控制唤醒 SQE，SQ 曾满时允许首次 submit 释放空间后再补。
    (void)arm_wake();
    // 单次 submit 可只提交部分 SQE；未提交部分仍由 ring 保留，不推断业务完成。
#if defined(FAIO_URING_CONTROL_TESTS) && FAIO_URING_CONTROL_TESTS
    const int first =
        control_test_submit(); // 独立单 TU 测试在真正 submit 边界注入一次背压。
#else
    const int first =
        ::io_uring_submit(&ring_); // 正常构建没有测试字段、分支或间接调用。
#endif
    // 只统计内核实际接受数，负返回不转换成无符号计数。
    std::size_t submitted = first > 0 ? static_cast<std::size_t>(first) : 0;
    // EINTR/EAGAIN/ENOMEM 保留重试责任，其他 enter 错误交宿主建立故障边界。
    int error =
        first < 0 && first != -EINTR && first != -EAGAIN && first != -ENOMEM
            ? -first
            : 0;
    // 首次 SQ 满未能 arm 时才补一次 submit，不循环刷空 SQ 或忙等完成。
    if (!error && arm_wake()) {
      // 补提控制 wake 与剩余 SQ；第二次同样保留 partial/backpressure 的语义。
#if defined(FAIO_URING_CONTROL_TESTS) && FAIO_URING_CONTROL_TESTS
      const int second =
          control_test_submit(); // 同一个 seam 覆盖 SQ 满后的补控制提交。
#else
      const int second =
          ::io_uring_submit(&ring_); // 正常构建仍直接使用 liburing。
#endif
      if (second > 0)
        submitted += static_cast<std::size_t>(second);
      else if (second < 0 && second != -EINTR && second != -EAGAIN &&
               second != -ENOMEM)
        error = -second;
    }
    // 累计真实提交量，与 prepare 接受的 native_submitted 分开记录。
    statistics_.native_flushed += submitted;
    // pending 表示仍有提交/取消责任，driver 后续必须继续推进而不能当作排空。
    return {submitted,
            ::io_uring_sq_ready(&ring_) != 0 || !pending_cancels_.empty(),
            error};
  }
  /** @brief 无EXT_ARG内核使用原生TIMEOUT；最多一个稳定timeout记录。 */
  int arm_wait_timer(int milliseconds) noexcept {
    if (timer_key_)
      return 0; // 前一个原生取消尚未排空；其CQE即会打断本次等待。
    auto *sqe = ::io_uring_get_sqe(&ring_);
    if (!sqe)
      return -EAGAIN; // 尚未获得SQ容量，不能无截止时间地进入无限GETEVENTS。
    // TIMEOUT 与普通请求使用同一不复用键空间，但不持有业务 token。
    const auto key = ++next_key_;
    if (!key || key >= wake_key_)
      std::terminate();
    try {
      // unordered_map 节点地址稳定，timespec 保持到原 TIMEOUT CQE 已消费。
      auto &timer = entries_.try_emplace(key).first->second;
      // 明确这是控制记录，消费后不能形成用户结果或 native_completed。
      timer.control_timeout = true;
      // 毫秒拆成合法秒/纳秒，内核借用的是节点内的结构而非栈临时量。
      timer.timeout = {milliseconds / 1000,
                       static_cast<long long>(milliseconds % 1000) * 1000000};
      // count=0 的原生超时负责结束 GETEVENTS，CQE 后才可销毁其参数。
      ::io_uring_prep_timeout(sqe, &timer.timeout, 0, 0);
      // 完成键连回稳定 TIMEOUT 节点，取消 ACK 另有独立节点。
      ::io_uring_sqe_set_data64(sqe, key);
      // 发布本次控制超时身份，保证同一时刻最多一个未排空 TIMEOUT。
      timer_key_ = key;
      return 0;
    } catch (...) {
      // 记录分配失败尚未借用 timespec，已经保留的 SQ 槽改为无 payload NOP。
      ::io_uring_prep_nop(sqe);
      ::io_uring_sqe_set_data64(sqe, 0);
      return -ENOMEM;
    }
  }
  /** @brief 提前返回等待时请求原生 TIMEOUT 取消，原超时 CQE 与 ACK 仍各自排空。
   */
  void cancel_wait_timer() noexcept {
    if (!timer_key_)
      return;
    // 可能已经消费超时；没有记录时不再产生迟到的控制取消。
    auto found = entries_.find(timer_key_);
    if (found == entries_.end() || found->second.cancel_requested)
      return;
    // 一次记录取得取消责任，重复 wake 不重复生成 ACK。
    found->second.cancel_requested = true;
    // 只排入原生控制队列；不在此释放稳定 timespec 或执行辅助线程任务。
    try {
      pending_cancels_.push_back(timer_key_);
    } catch (...) {
      std::terminate();
    } // 已接受控制参数也必须排空后再回收。
  }
  /** @brief SQ 锁内把持久取消责任准备为 ASYNC_CANCEL；容量不足保持原队列。
   * @details ACK 的完成键与 original key 不同，避免取消确认误删原 payload
   * 记录。
   */
  void issue_cancels() noexcept {
    while (!pending_cancels_.empty()) {
      // 队列保存原生完成键，而非可被复用的 fd 数字或请求地址。
      const auto original_key = pending_cancels_.front();
      const auto original = entries_.find(original_key);
      // 原 CQE 已真正消费时无需取消；只有此时才能撤销尚未提交的控制责任。
      if (original == entries_.end()) {
        pending_cancels_.pop_front();
        continue;
      }
      // 获得 SQ 槽前不弹队列；背压不会丢失取消。
      auto *sqe = ::io_uring_get_sqe(&ring_);
      if (!sqe)
        break;
      const auto ack_key = ++next_key_;
      if (!ack_key || ack_key >= wake_key_)
        std::terminate(); // 控制 key 同样不允许回绕碰撞原请求记录。
      const auto original_token =
          original->second.token; // 插入 ACK 可 rehash，先保存原记录值。
      try {
        // ACK 节点只保存 token 与角色，不保存原 payload 或协程地址。
        auto [ack, inserted] = entries_.try_emplace(ack_key);
        ack->second.token = original_token;
        // ACK 与业务结果由 harvest 分别编码，控制成功不能解除 original 租约。
        ack->second.ack = true;
      } catch (...) {
        std::terminate();
      }
      // 精确取消原 user_data 键；不能按数字 fd 误取消后来复用的资源。
      ::io_uring_prep_cancel64(sqe, original_key, 0);
      // 取消自身 CQE 返回独立 ACK 键，可能先于或晚于 original CQE。
      ::io_uring_sqe_set_data64(sqe, ack_key);
      // 只有 ACK 已稳定准备到 SQ 后才移除持久队列项。
      pending_cancels_.pop_front();
    }
  }
  /** @brief 唯一 CQ 消费者在 SQ 锁内有界解码真实 CQE，不直接调用协程消费者。
   * @param output 中立事件输出预算；尚未读取的普通 CQE 留在内核完成队列。
   * @return 本轮业务结果/ACK/释放事件数；控制 wake/TIMEOUT 不计入用户结果。
   */
  int harvest(std::span<backend_event> output) noexcept {
    // 输出数只计入交给域的事件，控制完成通过 control_progress_ 单独报告。
    std::size_t count = 0;
    std::array<io_uring_cqe *, 256>
        pending; // 固定指针批次，仅访问 liburing 真正填入的区间。
    // 普通/控制 CQE 共用最多 256 条总预算；控制不占 output
    // 结果数但仍占本轮消费预算。 使用 liburing 的 batch API 保留空 CQ 时
    // OVERFLOW/TASKRUN 推进，不直接读裸 tail。
    const auto available = ::io_uring_peek_batch_cqe(
        &ring_, pending.data(),
        static_cast<unsigned>(std::min(output.size(), pending.size())));
    for (unsigned index = 0; index < available; ++index) {
      auto *cqe = pending[index]; // 只借用真实 CQE 到整批
                                  // advance；未把地址交给业务消费者。
      // 读取整数完成键，避免内核保存或恢复悬空 awaiter 指针。
      const auto key = ::io_uring_cqe_get_data64(cqe);
      if (key == wake_key_) {
        wake_armed_ = false; // 唯一POLL_ADD已经真实完成，后续flush重新arm。
        // 控制完成足以结束本轮等待，但不会生成恢复业务协程的 result。
        control_progress_ = true;
        std::uint64_t ignored;
        // 非阻塞排空已合并的 eventfd 计数；下次 flush 再为后续远程通知 arm。
#if defined(FAIO_URING_CONTROL_TESTS) && FAIO_URING_CONTROL_TESTS
        control_test_step(control_test_point::harvest_before_drain);
#endif
        while (::read(wake_fd_, &ignored, sizeof(ignored)) > 0) {
        }
        // 这里只消费内核计数，绝不清 wake_pending_；drain
        // 期间的新通知仍须交回宿主。
#if defined(FAIO_URING_CONTROL_TESTS) && FAIO_URING_CONTROL_TESTS
        control_test_step(control_test_point::harvest_after_drain);
#endif
      }
      // 零键 NOP 或未登记控制值没有业务租约；普通键只命中后端稳定节点。
      const auto found = entries_.find(key);
      if (found != entries_.end() &&
          (found->second.control_timeout ||
           (found->second.ack && !found->second.token))) {
        // 原超时最后 CQE 到达后才清其身份，ACK 仍保留到自己的 CQE。
        if (found->second.control_timeout && timer_key_ == key)
          timer_key_ = 0;
        control_progress_ = true;
        entries_.erase(found); // 控制完成不冒充业务完成，也不恢复用户协程。
      } else if (found != entries_.end()) {
        auto &value = found->second;
        // ACK 只能证明取消命令完成，原请求返回才决定业务生命周期。
        auto kind = value.ack ? backend_event_kind::cancel_ack
                              : backend_event_kind::result;
        if (value.ack)
          ++statistics_.cancel_ack;
        // NOTIF 是零拷贝 buffer-release，不把它的 res 当成又一次发送字节数。
        else if (cqe->flags & IORING_CQE_F_NOTIF) {
          kind = backend_event_kind::buffer_release;
          ++statistics_.buffer_notifications;
        } else
          ++statistics_.native_completed;
        // 成功接收后回填实际地址长度；失败不读取未完成的内核输出元数据。
        if (value.request && value.request->kind == operation_kind::recvfrom &&
            cqe->res >= 0 && value.request->output_address_length)
          *value.request->output_address_length =
              value.request->message.msg_namelen;
        // 描述数组由请求拥有；只把 msghdr 的输出字段写回原调用方描述对象。
        if (value.request && value.request->kind == operation_kind::recvmsg &&
            cqe->res >= 0) {
          auto &r = *value.request;
          // 回填源地址真实长度，不改调用方提供的地址存储指针。
          r.output_message->msg_namelen = r.message.msg_namelen;
          // 回填实际辅助数据长度，供上层检测容量和截断。
          r.output_message->msg_controllen = r.message.msg_controllen;
          // 保留 MSG_TRUNC/MSG_CTRUNC 等内核结果，不能以输入 flags 覆盖。
          r.output_message->msg_flags = r.message.msg_flags;
        }
        // MORE 意味原生引用可能继续存在，发送结果可保存但不能提前归还 payload。
        const bool more = !value.ack && (cqe->flags & IORING_CQE_F_MORE);
        // 中立事件保留完整代际、真实 res 与 MORE；域随后统一仲裁取消与恢复。
        output[count++] = {kind, value.token, cqe->res, more ? 1u : 0u};
        // 没有 MORE 的最终事件才移除该节点；原请求与 ACK 的节点分别回收。
        if (!more) {
          if (value.sigpipe_sensitive)
            --sigpipe_operations_;
          if (value.request)
            // 清掉请求键，未来取消回调不能重新命中已消费的原生记录。
            value.request->native_completion_key = 0;
          entries_.erase(found); // 最后 CQE 已解除原生 payload 引用；ACK
                                 // 独立保留到自身 CQE。
        }
      }
      // 本条的 res/flags 及输出元数据已复制；其 CQ 槽由整批唯一 head
      // 更新统一归还。
    }
    // 唯一 CQ 消费者完成整批解码以后才 release head，之后不再读取任何 pending
    // 指针。
    ::io_uring_cq_advance(
        &ring_,
        available); // 一批一次 head 更新，保留 MORE/NOTIF/ACK 的逐条租约协议。
    return static_cast<int>(count);
  }
#if defined(FAIO_URING_CONTROL_TESTS) && FAIO_URING_CONTROL_TESTS
  // 仅 tests/faio_uring_control_tests 的唯一 TU 启用；不与其他配置的 TU
  // 链接，避免 ODR 分歧。
  friend struct uring_control_test_access;
  enum class control_test_point {
    poll_after_load,
    poll_after_consume,
    poll_before_wait,
    wake_after_exchange,
    wake_before_write,
    harvest_before_drain,
    harvest_after_drain
  };
  void control_test_step(control_test_point point) const noexcept {
    if (control_test_step_)
      control_test_step_(point, control_test_context_);
  }
  int control_test_submit() noexcept {
    return control_test_submit_
               ? control_test_submit_(&ring_, control_test_context_)
               : ::io_uring_submit(&ring_);
  }
  int (*control_test_submit_)(io_uring *, void *) noexcept {};
  int (*control_test_wait_)(void *) noexcept {};
  void (*control_test_step_)(control_test_point, void *) noexcept {};
  ssize_t (*control_test_write_)(int, const void *, std::size_t,
                                 void *) noexcept {};
  void *control_test_context_{};
#endif
  static constexpr std::uint64_t wake_key_ =
      UINT64_MAX; ///< 唯一控制 wake 键，普通键禁止回绕到此值。
  mutable std::atomic_bool wake_pending_{
      false}; ///< 通知交接独立于 eventfd drain；const wake 可跨线程。
  bool wake_armed_{},
      control_progress_{}; ///< 常驻 wake 是否已准备、本轮是否消费了控制完成。
  std::uint64_t
      timer_key_{}; ///< 旧内核原生 TIMEOUT 的唯一在途身份，最终 CQE 后归零。
  io_uring ring_{}; ///< 私有 SQ/CQ 内核映射，只有本后端拥有退出责任。
  int wake_fd_{-1}; ///< 仅用于控制通知，不接管业务 fd 或借用业务 buffer。
  bool initialized_{};
  bool
      extended_wait_{}; ///< 实际EXT_ARG能力；false也用于测试原生TIMEOUT兼容路径。
  bool socket_commands_{};
  bool accept_nowait_{};
  std::array<bool, 256> supported_{};
  mutable std::mutex
      mutex_; ///< 串行保护 SQ/记录/取消；GETEVENTS 睡眠不持有此锁。
  std::uint64_t
      next_key_{}; ///< 单调完成键，避免迟到 CQE 的 ABA；耗尽时禁止复用。
  // 声明在 entries_ 前面，保证析构顺序先销毁所有节点/bucket，再释放其内存资源。
  // SQ/CQ/取消访问全部由 mutex_ 串行，因此私有池无需第二把锁或辅助线程。
  std::pmr::unsynchronized_pool_resource
      entry_memory_; ///< 节点按真实在途高水位复用，稳态不逐条 malloc/free。
  std::pmr::unordered_map<std::uint64_t, entry> entries_{
      &entry_memory_}; ///< 地址稳定的原请求/ACK/TIMEOUT
                       ///< 节点，仍以不复用整数键查找。
  std::deque<std::uint64_t>
      pending_cancels_; ///< 尚未准备到 SQ 的取消责任，背压时保留。
  backend_statistics statistics_;
  std::size_t sigpipe_operations_{}; ///< 尚未消费最终 CQE 的敏感 WRITE
                                     ///< 数，决定原生窗口信号保护。
};
} // namespace faio::io::detail
#endif
