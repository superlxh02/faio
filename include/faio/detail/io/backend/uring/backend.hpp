/**
 * @file faio/detail/io/backend/uring/backend.hpp
 * @brief io_uring 原生异步后端：后端封装与提交协议。
 */
#pragma once
#if defined(__linux__) && defined(FAIO_HAS_IO_URING) && FAIO_HAS_IO_URING
#include "faio/detail/io/protocol/backend.hpp"
#include "faio/detail/io/platform/linux/backend_selection.hpp"
#include "faio/detail/io/core/resource_state.hpp"
#include "faio/detail/io/protocol/request.hpp"
#include "faio/detail/io/testing/uring_control_policy.hpp"
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
/**
 * @brief Linux 原生异步后端；SQ/CQ、取消确认与零拷贝释放共用稳定的代际协议。
 * @details user_data 是不复用的整数完成键，用于查找后端持有的记录，不保存协程地址。
 * 接受请求后即持有内核引用，最终 CQE/NOTIF 到达后才释放请求与缓冲区租约。
 * 跨线程 submit/cancel 通过短锁串行化；等待使用 ring fd 的原生 GETEVENTS，
 * eventfd 仅对应一个用于控制唤醒的 POLL_ADD SQE，等待时不持有锁。
 */
class uring_backend {
 public:
  static constexpr bool native_proactor = true; /**< 是否直接提交并接收原生异步操作完成。 */
  static constexpr bool poll_flushes_submissions =
      true; /**< poll 首先提交 SQ/取消责任，无需域重复 flush。 */
  static constexpr const char* backend_name = "uring"; /**< 后端名称常量，用于能力报告与诊断。 */

  /**
   *  @brief 建立私有 ring、控制 eventfd 和只读能力缓存；不注册业务 fd 的
   * readiness。
   * @param capacity 请求的 SQ 容量，最终容量由内核和 CLAMP 约束。
   * @param use_extended_wait 是否使用探测到的 EXT_ARG；false 保留原生 TIMEOUT
   * 兼容路径。
   * @throws std::system_error
   * 任一原生控制资源初始化失败；构造阶段不接受业务请求。
   */
  explicit uring_backend(unsigned capacity = 256, bool use_extended_wait = true) {
    const auto preheated_entries = static_cast<std::size_t>(capacity) * 2 + 2;
    entries_.reserve(preheated_entries);
    for (std::size_t index = 1; index <= preheated_entries; ++index)
      entries_.try_emplace(index);
    entries_.clear();
    io_uring_params params{};
    params.flags = IORING_SETUP_CLAMP;
    const int result = ::io_uring_queue_init_params(capacity, &ring_, &params);
    if (result < 0)
      throw std::system_error(-result,
                              std::generic_category(),
                              "io_uring_queue_init: 原生后端初始化失败；使用 epoll 请重新构建 "
                              "FAIO_ENABLE_IO_URING=OFF");
    initialized_ = true;
    extended_wait_ = use_extended_wait && (params.features & IORING_FEAT_EXT_ARG);
    wake_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake_fd_ < 0) {
      const int error = errno;
      ::io_uring_queue_exit(&ring_);
      initialized_ = false;
      throw std::system_error(error, std::generic_category(), "uring eventfd");
    }
    auto* probe = ::io_uring_get_probe_ring(&ring_);
    if (!probe) {
      ::close(wake_fd_);
      ::io_uring_queue_exit(&ring_);
      initialized_ = false;
      throw std::system_error(ENOMEM, std::generic_category(), "io_uring opcode probe");
    }
    for (unsigned i = 0; i < supported_.size(); ++i)
      supported_[i] = ::io_uring_opcode_supported(probe, static_cast<int>(i));
    ::io_uring_free_probe(probe);
    utsname info{};
    if (::uname(&info) == 0) {
      try {
        const auto version = parse_kernel_version(info.release);
        socket_commands_ = version.major > 6 || (version.major == 6 && version.minor >= 7);
        accept_nowait_ = version.major > 6 || (version.major == 6 && version.minor >= 10);
      } catch (const std::invalid_argument&) {
        socket_commands_ = false;
      }
    }
  }

  /** @brief 销毁 Linux io_uring 原生完成后端。 */
  ~uring_backend() {
    if (initialized_)
      ::io_uring_queue_exit(&ring_);
    if (wake_fd_ >= 0)
      ::close(wake_fd_);
  }

  /** @brief 禁止复制构造 Linux io_uring 原生完成后端。 */
  uring_backend(const uring_backend&) = delete;

  /** @brief 返回实际后端用于诊断的静态名称。 */
  const char* name() const noexcept { return backend_name; }

  /** @brief 原生普通 fd 请求直接进入 SQE，无需额外的长期 readiness 注册。 */
  int register_resource(int, std::uint64_t) noexcept { return 0; }

  /** @brief 本后端未安装 fd readiness；原生操作的引用通过最终 CQE 独立排空。 */
  void unregister_resource(int) noexcept {}

  /**
   * @brief 查询构造时固定的 opcode/flag 能力，不调用业务 syscall
   * 或启动服务线程。
   */
  bool supports(std::uint32_t kind) const noexcept {
    const auto operation = static_cast<operation_kind>(kind);
    if (operation == operation_kind::ACCEPT_NOWAIT && !accept_nowait_)
      return false;
    if ((operation == operation_kind::GETSOCKOPT || operation == operation_kind::SETSOCKOPT
         || operation == operation_kind::SOCKET_INQ || operation == operation_kind::SOCKET_OUTQ)
        && !socket_commands_)
      return false;

    const int opcode = opcode_for(static_cast<operation_kind>(kind));
    return opcode >= 0 && static_cast<unsigned>(opcode) < supported_.size() && supported_[opcode];
  }

  /** @brief 读取后端累计的提交、完成和控制事件统计。 */
  backend_statistics statistics() const noexcept {
    std::lock_guard lock(mutex_);
    return statistics_;
  }

  /**
   *  @brief SQ 满只返回 would_queue；尚未接管时调用者可安全保存在有界域队列。
   * @details 一旦 accepted，即使 submit 尚未进入内核也不可立即释放 request。
   */
  backend_submit_result try_submit(backend_operation operation) noexcept {
    std::lock_guard lock(mutex_);
    auto& request = *operation.request;
    if (!supports(static_cast<std::uint32_t>(request.kind)))
      return {backend_submit_status::REJECTED, EOPNOTSUPP};
    if (!pending_cancels_.empty())
      issue_cancels();

    auto* sqe = ::io_uring_get_sqe(&ring_);
    if (!sqe)
      return {backend_submit_status::WOULD_QUEUE, 0};
    const auto key = ++next_key_;
    if (!key || key >= wake_key_)
      std::terminate();
    try {
      auto [it, inserted] = entries_.try_emplace(key);
      auto& entry = it->second;
      entry.token = operation.token;
      entry.request = &request;
      entry.sigpipe_sensitive =
          request.resource && !request.resource->regular_file
          && (request.kind == operation_kind::WRITE || request.kind == operation_kind::WRITEV);
      prepare(*sqe, request, entry);
      request.native_completion_key = key;
      if (entry.sigpipe_sensitive)
        sqe->flags |= IOSQE_ASYNC;

      if (entry.sigpipe_sensitive)
        ++sigpipe_operations_;
    } catch (...) {
      ::io_uring_prep_nop(sqe);
      ::io_uring_sqe_set_data64(sqe, 0);
      entries_.erase(key);
      request.native_completion_key = 0;
      return {backend_submit_status::REJECTED, ENOMEM};
    }
    ::io_uring_sqe_set_data64(sqe, key);
    ++statistics_.native_submitted;

    return {backend_submit_status::ACCEPTED, 0};
  }

  /**
   *  @brief 短暂持有 SQ 锁并推进一次提交；返回的 submitted 是内核实际接受数量。
   * @details 信号保护只覆盖可能执行管道/流 WRITE 的 enter/task_work 窗口。
   */
  backend_flush_result flush() noexcept {
    std::lock_guard lock(mutex_);
    sigpipe_scope protected_signals{sigpipe_operations_ != 0};
    return flush_locked();
  }

  /**  @brief 取消确认不是业务完成；原请求 CQE 到达以前一直保留内核租约。 */
  void request_cancel(backend_operation operation) noexcept {
    std::lock_guard lock(mutex_);
    if (!operation.request)
      return;
    const auto& request = *operation.request;
    const auto original = entries_.find(request.native_completion_key);
    if (original == entries_.end() || original->second.token != operation.token
        || original->second.cancel_requested)
      return;
    original->second.cancel_requested = true;
    try {
      pending_cancels_.push_back(original->first);
    } catch (...) {
      std::terminate();
    }
    issue_cancels();
    wake();
  }

  /**
   * @brief 作为唯一 CQ 消费者，直接通过原生 GETEVENTS 等待，不使用业务 fd 的就绪事件模拟。
   * @details eventfd 仅对应一个用于控制唤醒的原生 POLL_ADD SQE；远程 submit/cancel
   * 先准备请求，再写入 eventfd。等待时不持 SQ 锁；enter 的 to_submit 为 0，
   * 不读取远程写入的 SQ。新内核通过 EXT_ARG 传入栈上的 timeout；
   * 5.10 内核缺少 EXT_ARG 时，使用持有独立参数的 TIMEOUT SQE。
   */
  int poll(std::span<backend_event> output, std::optional<int> timeout) noexcept {
    bool protect_wait;
    int count;
    {
      std::lock_guard lock(mutex_);
      protect_wait = sigpipe_operations_ != 0;
      sigpipe_scope signals{protect_wait};
      const bool pending_notice = wake_pending_.load(std::memory_order_seq_cst);

#if defined(FAIO_URING_CONTROL_TESTS) && FAIO_URING_CONTROL_TESTS
      control_test_step(control_test_point::POLL_AFTER_LOAD);
#endif
      const bool noticed =
          pending_notice && wake_pending_.exchange(false, std::memory_order_seq_cst);
#if defined(FAIO_URING_CONTROL_TESTS) && FAIO_URING_CONTROL_TESTS
      control_test_step(control_test_point::POLL_AFTER_CONSUME);
#endif
      control_progress_ = false;
      const auto flushed = flush_locked();
      if (flushed.error)
        return -flushed.error;
      count = harvest(output);
      if (count || control_progress_ || noticed || flushed.pending || (timeout && !*timeout))
        return count;
      if (timeout && !extended_wait_) {
        const int prepared = arm_wait_timer(*timeout);
        if (prepared < 0)
          return 0;
        const auto timer_flushed = flush_locked();
        if (timer_flushed.error)
          return -timer_flushed.error;
        if (timer_flushed.pending)
          return harvest(output);
      }
    }
    sigpipe_scope protected_wait{protect_wait};
    int waited;
#if defined(FAIO_URING_CONTROL_TESTS) && FAIO_URING_CONTROL_TESTS
    control_test_step(control_test_point::POLL_BEFORE_WAIT);
#endif
    do {
#if defined(FAIO_URING_CONTROL_TESTS) && FAIO_URING_CONTROL_TESTS
      if (control_test_)
        waited = control_test_.wait();
      else
#endif
          if (timeout && extended_wait_) {
        __kernel_timespec duration{*timeout / 1000,
                                   static_cast<long long>(*timeout % 1000) * 1000000};
        io_uring_getevents_arg argument{};
        argument.ts = reinterpret_cast<std::uintptr_t>(&duration);
        waited = ::io_uring_enter2(ring_.ring_fd,
                                   0,
                                   1,
                                   IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG,
                                   reinterpret_cast<sigset_t*>(&argument),
                                   sizeof(argument));
      } else {
        waited = ::io_uring_enter(ring_.ring_fd, 0, 1, IORING_ENTER_GETEVENTS, nullptr);
      }
    } while (waited == -EINTR);
    std::lock_guard lock(mutex_);
    sigpipe_scope signals{sigpipe_operations_ != 0};
    count = harvest(output);
    cancel_wait_timer();
    const auto flushed = flush_locked();
    if (flushed.error)
      return -flushed.error;
    if (waited < 0 && waited != -ETIME && waited != -EAGAIN && waited != -ENOMEM)
      return waited;
    return count;
  }

  /**
   * @brief 合并同一入口 epoch 内的控制通知；业务请求与取消仍保存在稳定的 SQ 和记录中。
   * @details 首个将状态从 false 改为 true 的发布者负责写入 eventfd，后续通知由同一
   * latch 合并。poll 入口消费 latch 后必须立即返回宿主，即使旧 wake CQE 同时排空
   * 了新计数。完成和取消结果仍只来自原生 CQE，不使用业务就绪事件或辅助线程。
   */
  void wake() const noexcept {
    const bool already_pending = wake_pending_.exchange(true, std::memory_order_seq_cst);
#if defined(FAIO_URING_CONTROL_TESTS) && FAIO_URING_CONTROL_TESTS
    control_test_step(control_test_point::WAKE_AFTER_EXCHANGE);
#endif
    if (already_pending)
      return;
    const std::uint64_t one = 1;
    ssize_t written;
#if defined(FAIO_URING_CONTROL_TESTS) && FAIO_URING_CONTROL_TESTS
    control_test_step(control_test_point::WAKE_BEFORE_WRITE);
#endif
    do {
#if defined(FAIO_URING_CONTROL_TESTS) && FAIO_URING_CONTROL_TESTS
      written = control_test_ ? control_test_.write(wake_fd_, &one, sizeof(one))
                              : ::write(wake_fd_, &one, sizeof(one));
#else
      written = ::write(wake_fd_, &one, sizeof(one));
#endif
    } while (written < 0 && errno == EINTR);
    (void)written;
  }

  /** @brief 停机只打断等待；已接受原生操作仍须由域取消并消费最后一个内核事件。 */
  void begin_shutdown() noexcept { wake(); }

  /**
   *  @brief 私有 ring 的永久 enter 故障不能以 close(fd) 伪装已解除内核引用。
   * @details EINTR/EAGAIN/ENOMEM
   * 已归为可重试背压；其他永久驱动错误意味着完成协议损坏。 Linux close ring
   * 可异步清理，返回 false 要求宿主 fail-fast，绝不提前恢复借用者。
   */
  bool begin_failure(int) noexcept { return false; }

  /**
   *  @brief 查询原生业务/ACK/TIMEOUT 与待提交 SQ 是否都已排空。
   * @details 唯一常驻控制 wake 不持有业务 payload；ring
   * 析构仍由域停机纪律负责。
   */
  bool quiescent() const noexcept {
    std::lock_guard lock(mutex_);
    return entries_.empty() && pending_cancels_.empty() && ::io_uring_sq_ready(&ring_) == 0;
  }

 private:
  /**
   *  @brief io_uring 在提交线程内执行管道 WRITE 时仍可发 SIGPIPE。
   * @details 只修改当前线程掩码，在 enter/可触发 task_work 的 CQ
   * 读取窗口内屏蔽； 消费本窗口新产生的 pending SIGPIPE，保留调用前 pending
   * 信号和原掩码。 网络 SEND 使用 MSG_NOSIGNAL，不增加线程信号
   * syscall；文件同样无需此保护。
   */
  class sigpipe_scope {
   public:
    /** @brief 构造 SIGPIPE 屏蔽作用域。 */
    explicit sigpipe_scope(bool enabled) noexcept : enabled_(enabled) {
      if (!enabled_)
        return;
      ::sigemptyset(&signal_);
      ::sigaddset(&signal_, SIGPIPE);
      if (::pthread_sigmask(SIG_BLOCK, &signal_, &previous_) != 0)
        std::terminate();
      sigset_t pending{};
      (void)::sigpending(&pending);
      already_pending_ = ::sigismember(&pending, SIGPIPE) == 1;
    }

    /** @brief 销毁 SIGPIPE 屏蔽作用域。 */
    ~sigpipe_scope() {
      if (!enabled_)
        return;
      if (!already_pending_) {
        sigset_t pending{};
        (void)::sigpending(&pending);
        if (::sigismember(&pending, SIGPIPE) == 1) {
          timespec immediately{};
          (void)::sigtimedwait(&signal_, nullptr, &immediately);
        }
      }
      (void)::pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
    }

   private:
    sigset_t signal_{}, previous_{}; /**< 进入信号保护前的线程信号屏蔽集合，退出时完整恢复。 */
    bool enabled_{},
        already_pending_{}; /**< 进入保护前信号是否已经待处理，退出时避免误消费旧信号。 */
  };

  /** @brief 后端登记项。 */
  struct entry {
    std::uint64_t token{}; /**< 域操作完整代际；ACK 同样保留它但不据此释放原请求。 */
    io_request* request{}; /**< 指向稳定域槽，在原请求最后一个 CQE 到达前由域租约固定。 */
    open_how how{};        /**< openat2 的扩展参数由后端记录拥有，unordered_map 不移动节点地址。 */
    __kernel_timespec timeout{}; /**< 5.10 内核的原生 TIMEOUT 参数，在最终 CQE 到达前保持稳定。 */
    bool ack{}, cancel_requested{}, awaiting_notification{}, sigpipe_sensitive{},
        control_timeout{}; /**< 条目是否属于控制超时，区别于业务操作的超时终态。 */
  };

  /** @brief 只作 operation 到真实内核 opcode 的映射；缺项在接受前明确拒绝。 */
  static int opcode_for(operation_kind kind) noexcept {
    switch (kind) {
      case operation_kind::RECV:
        return IORING_OP_RECV;
      case operation_kind::SEND:
        return IORING_OP_SEND;
      case operation_kind::RECVFROM:
      case operation_kind::RECVMSG:
        return IORING_OP_RECVMSG;
      case operation_kind::SENDTO:
      case operation_kind::SENDMSG:
        return IORING_OP_SENDMSG;
      case operation_kind::SEND_ZC:
        return IORING_OP_SEND_ZC;
      case operation_kind::SENDMSG_ZC:
        return IORING_OP_SENDMSG_ZC;
      case operation_kind::CONNECT:
        return IORING_OP_CONNECT;
      case operation_kind::ACCEPT:
      case operation_kind::ACCEPT_NOWAIT:
        return IORING_OP_ACCEPT;
      case operation_kind::READ:
        return IORING_OP_READ;
      case operation_kind::WRITE:
        return IORING_OP_WRITE;
      case operation_kind::READV:
        return IORING_OP_READV;
      case operation_kind::WRITEV:
        return IORING_OP_WRITEV;
      case operation_kind::OPEN:
        return IORING_OP_OPENAT;
      case operation_kind::OPEN2:
        return IORING_OP_OPENAT2;
      case operation_kind::FSYNC:
        return IORING_OP_FSYNC;
      case operation_kind::CLOSE:
        return IORING_OP_CLOSE;
      case operation_kind::SHUTDOWN:
        return IORING_OP_SHUTDOWN;
      case operation_kind::SOCKET:
        return IORING_OP_SOCKET;
      case operation_kind::READY:
        return IORING_OP_POLL_ADD;
      case operation_kind::GETSOCKOPT:
      case operation_kind::SETSOCKOPT:
      case operation_kind::SOCKET_INQ:
      case operation_kind::SOCKET_OUTQ:
        return IORING_OP_URING_CMD;
      case operation_kind::STATX:
        return IORING_OP_STATX;
      case operation_kind::MKDIRAT:
        return IORING_OP_MKDIRAT;
      case operation_kind::UNLINKAT:
        return IORING_OP_UNLINKAT;
      case operation_kind::RENAMEAT:
        return IORING_OP_RENAMEAT;
      case operation_kind::LINKAT:
        return IORING_OP_LINKAT;
      case operation_kind::SYMLINKAT:
        return IORING_OP_SYMLINKAT;
      case operation_kind::FTRUNCATE:
        return IORING_OP_FTRUNCATE;
      default:
        return -1;
    }
  }

  /**
   *  @brief 稳定 typed request 直接准备原生 SQE；此函数不
   * submit、不挂起或恢复协程。
   * @param sqe 已保留但尚未提交的 SQ 槽，调用方持有 SQ 锁。
   * @param r 由域持有的请求，路径、地址及 iovec 描述在最后 CQE 前保持稳定。
   * @param owner 后端节点拥有 open_how 等扩展参数；借用 payload 仍由外层 lease
   * 固定。
   * @details 数据 IO 使用对应原生 opcode；只有用户显式 ready 才准备 POLL_ADD。
   */
  static void prepare(io_uring_sqe& sqe, io_request& r, entry& owner) {
    const auto length = static_cast<unsigned>(std::min<std::size_t>(r.length, UINT_MAX));
    switch (r.kind) {
      case operation_kind::RECV:
        ::io_uring_prep_recv(&sqe, r.fd, r.buffer, length, r.flags);
        break;
      case operation_kind::SEND:
        ::io_uring_prep_send(&sqe, r.fd, r.const_buffer, length, r.flags | MSG_NOSIGNAL);
        break;
      case operation_kind::SEND_ZC:
        ::io_uring_prep_send_zc(&sqe,
                                r.fd,
                                r.const_buffer,
                                length,
                                r.flags | MSG_NOSIGNAL,
                                static_cast<unsigned>(r.argument));
        break;
      case operation_kind::RECVFROM:
        r.scalar_vector = {r.buffer, r.length};
        r.message = {};
        r.message.msg_name = r.output_address;
        r.message.msg_namelen = r.output_address_length ? *r.output_address_length : 0;
        r.message.msg_iov = &r.scalar_vector;
        r.message.msg_iovlen = 1;
        ::io_uring_prep_recvmsg(&sqe, r.fd, &r.message, r.flags);
        break;
      case operation_kind::RECVMSG:
        r.message = *r.output_message;
        if (r.message.msg_iovlen)
          r.vectors.assign(r.message.msg_iov, r.message.msg_iov + r.message.msg_iovlen);
        r.message.msg_iov = r.vectors.data();
        ::io_uring_prep_recvmsg(&sqe, r.fd, &r.message, r.flags);
        break;
      case operation_kind::SENDTO:
        r.scalar_vector = {const_cast<void*>(r.const_buffer), r.length};
        r.message = {};
        r.message.msg_name = &r.address;
        r.message.msg_namelen = r.address_length;
        r.message.msg_iov = &r.scalar_vector;
        r.message.msg_iovlen = 1;
        ::io_uring_prep_sendmsg(&sqe, r.fd, &r.message, r.flags | MSG_NOSIGNAL);
        break;
      case operation_kind::SENDMSG:
        r.message.msg_iov = r.vectors.data();
        ::io_uring_prep_sendmsg(&sqe, r.fd, &r.message, r.flags | MSG_NOSIGNAL);
        break;
      case operation_kind::SENDMSG_ZC:
        r.message.msg_iov = r.vectors.data();
        ::io_uring_prep_sendmsg_zc(&sqe, r.fd, &r.message, r.flags | MSG_NOSIGNAL);
        break;
      case operation_kind::CONNECT:
        ::io_uring_prep_connect(
            &sqe, r.fd, reinterpret_cast<sockaddr*>(&r.address), r.address_length);
        break;
      case operation_kind::ACCEPT:
      case operation_kind::ACCEPT_NOWAIT:
        ::io_uring_prep_accept(&sqe,
                               r.fd,
                               r.output_address,
                               r.output_address_length,
                               r.flags | SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (r.kind == operation_kind::ACCEPT_NOWAIT)
          sqe.ioprio |= IORING_ACCEPT_DONTWAIT;
        break;
      case operation_kind::READ:
        ::io_uring_prep_read(&sqe,
                             r.fd,
                             r.buffer,
                             length,
                             r.resource && !r.resource->regular_file ? UINT64_MAX : r.offset);
        break;
      case operation_kind::WRITE:
        ::io_uring_prep_write(&sqe,
                              r.fd,
                              r.const_buffer,
                              length,
                              r.resource && !r.resource->regular_file ? UINT64_MAX : r.offset);
        break;
      case operation_kind::READV:
        ::io_uring_prep_readv2(&sqe,
                               r.fd,
                               r.vectors.data(),
                               static_cast<unsigned>(r.vectors.size()),
                               r.resource && !r.resource->regular_file ? UINT64_MAX : r.offset,
                               r.flags);
        break;
      case operation_kind::WRITEV:
        ::io_uring_prep_writev2(&sqe,
                                r.fd,
                                r.vectors.data(),
                                static_cast<unsigned>(r.vectors.size()),
                                r.resource && !r.resource->regular_file ? UINT64_MAX : r.offset,
                                r.flags);
        break;
      case operation_kind::OPEN:
        ::io_uring_prep_openat(
            &sqe, r.fd, r.path.c_str(), r.flags | O_CLOEXEC, static_cast<mode_t>(r.argument));
        break;
      case operation_kind::OPEN2:
        owner.how = {r.extension_flags | O_CLOEXEC, r.extension_mode, r.extension_resolve};
        ::io_uring_prep_openat2(&sqe, r.fd, r.path.c_str(), &owner.how);
        break;
      case operation_kind::FSYNC:
        ::io_uring_prep_fsync(&sqe, r.fd, r.flags);
        break;
      case operation_kind::CLOSE:
        ::io_uring_prep_close(&sqe, r.fd);
        break;
      case operation_kind::SHUTDOWN:
        ::io_uring_prep_shutdown(&sqe, r.fd, r.argument);
        break;
      case operation_kind::SOCKET:
        ::io_uring_prep_socket(
            &sqe, r.argument, r.argument2 | SOCK_NONBLOCK | SOCK_CLOEXEC, r.argument3, r.flags);
        break;
      case operation_kind::READY: {
        unsigned flags = POLLERR | POLLHUP;
        if (r.argument & 1)
          flags |= POLLIN;
        if (r.argument & 2)
          flags |= POLLOUT;
        ::io_uring_prep_poll_add(&sqe, r.fd, flags);
        break;
      }
      case operation_kind::GETSOCKOPT:
        ::io_uring_prep_cmd_sock(&sqe,
                                 SOCKET_URING_OP_GETSOCKOPT,
                                 r.fd,
                                 r.argument,
                                 r.argument2,
                                 r.buffer,
                                 static_cast<int>(r.length));
        break;
      case operation_kind::SETSOCKOPT:
        ::io_uring_prep_cmd_sock(&sqe,
                                 SOCKET_URING_OP_SETSOCKOPT,
                                 r.fd,
                                 r.argument,
                                 r.argument2,
                                 const_cast<void*>(r.const_buffer),
                                 static_cast<int>(r.length));
        break;
      case operation_kind::SOCKET_INQ:
        ::io_uring_prep_cmd_sock(&sqe, SOCKET_URING_OP_SIOCINQ, r.fd, 0, 0, nullptr, 0);
        break;
      case operation_kind::SOCKET_OUTQ:
        ::io_uring_prep_cmd_sock(&sqe, SOCKET_URING_OP_SIOCOUTQ, r.fd, 0, 0, nullptr, 0);
        break;
      case operation_kind::STATX:
        ::io_uring_prep_statx(&sqe,
                              r.fd,
                              r.path.c_str(),
                              r.flags,
                              static_cast<unsigned>(r.argument),
                              static_cast<struct statx*>(r.buffer));
        break;
      case operation_kind::MKDIRAT:
        ::io_uring_prep_mkdirat(&sqe, r.fd, r.path.c_str(), static_cast<mode_t>(r.argument));
        break;
      case operation_kind::UNLINKAT:
        ::io_uring_prep_unlinkat(&sqe, r.fd, r.path.c_str(), r.flags);
        break;
      case operation_kind::RENAMEAT:
        ::io_uring_prep_renameat(&sqe,
                                 r.fd,
                                 r.path.c_str(),
                                 r.argument2,
                                 r.path2.c_str(),
                                 static_cast<unsigned>(r.flags));
        break;
      case operation_kind::LINKAT:
        ::io_uring_prep_linkat(&sqe, r.fd, r.path.c_str(), r.argument2, r.path2.c_str(), r.flags);
        break;
      case operation_kind::SYMLINKAT:
        ::io_uring_prep_symlinkat(&sqe, r.path.c_str(), r.fd, r.path2.c_str());
        break;
      case operation_kind::FTRUNCATE:
        ::io_uring_prep_ftruncate(&sqe, r.fd, static_cast<loff_t>(r.offset));
        break;
      default:
        ::io_uring_prep_nop(&sqe);
        break;
    }
  }

  /**  @brief 在 SQ 锁内准备唯一的原生控制唤醒，不用于观察业务 socket 或文件就绪。 */
  bool arm_wake() noexcept {
    if (wake_armed_)
      return false;
    auto* sqe = ::io_uring_get_sqe(&ring_);
    if (!sqe)
      return false;
    ::io_uring_prep_poll_add(sqe, wake_fd_, POLLIN);
    ::io_uring_sqe_set_data64(sqe, wake_key_);
    wake_armed_ = true;
    return true;
  }

  /**  @brief 执行一次普通 submit；若 SQ 曾满，再补交一个控制 wake，不阻塞协程 worker。 */
  backend_flush_result flush_locked() noexcept {
    if (!pending_cancels_.empty())
      issue_cancels();
    (void)arm_wake();
#if defined(FAIO_URING_CONTROL_TESTS) && FAIO_URING_CONTROL_TESTS
    const int first = control_test_submit();
#else
    const int first = ::io_uring_submit(&ring_);
#endif
    std::size_t submitted = first > 0 ? static_cast<std::size_t>(first) : 0;
    int error = first < 0 && first != -EINTR && first != -EAGAIN && first != -ENOMEM ? -first : 0;
    if (!error && arm_wake()) {
#if defined(FAIO_URING_CONTROL_TESTS) && FAIO_URING_CONTROL_TESTS
      const int second = control_test_submit();
#else
      const int second = ::io_uring_submit(&ring_);
#endif
      if (second > 0)
        submitted += static_cast<std::size_t>(second);
      else if (second < 0 && second != -EINTR && second != -EAGAIN && second != -ENOMEM)
        error = -second;
    }
    statistics_.native_flushed += submitted;
    return {submitted, ::io_uring_sq_ready(&ring_) != 0 || !pending_cancels_.empty(), error};
  }

  /**  @brief 缺少 EXT_ARG 的内核使用原生 TIMEOUT，最多保留一条地址稳定的超时记录。 */
  int arm_wait_timer(int milliseconds) noexcept {
    if (timer_key_)
      return 0;
    auto* sqe = ::io_uring_get_sqe(&ring_);
    if (!sqe)
      return -EAGAIN;
    const auto key = ++next_key_;
    if (!key || key >= wake_key_)
      std::terminate();
    try {
      auto& timer = entries_.try_emplace(key).first->second;
      timer.control_timeout = true;
      timer.timeout = {milliseconds / 1000, static_cast<long long>(milliseconds % 1000) * 1000000};
      ::io_uring_prep_timeout(sqe, &timer.timeout, 0, 0);
      ::io_uring_sqe_set_data64(sqe, key);
      timer_key_ = key;
      return 0;
    } catch (...) {
      ::io_uring_prep_nop(sqe);
      ::io_uring_sqe_set_data64(sqe, 0);
      return -ENOMEM;
    }
  }

  /**  @brief 提前返回等待时请求原生 TIMEOUT 取消，原超时 CQE 与 ACK 仍各自排空。 */
  void cancel_wait_timer() noexcept {
    if (!timer_key_)
      return;
    auto found = entries_.find(timer_key_);
    if (found == entries_.end() || found->second.cancel_requested)
      return;
    found->second.cancel_requested = true;
    try {
      pending_cancels_.push_back(timer_key_);
    } catch (...) {
      std::terminate();
    }
  }

  /**
   *  @brief SQ 锁内把持久取消责任准备为 ASYNC_CANCEL；容量不足保持原队列。
   * @details ACK 的完成键与 original key 不同，避免取消确认误删原 payload
   * 记录。
   */
  void issue_cancels() noexcept {
    while (!pending_cancels_.empty()) {
      const auto original_key = pending_cancels_.front();
      const auto original = entries_.find(original_key);
      if (original == entries_.end()) {
        pending_cancels_.pop_front();
        continue;
      }
      auto* sqe = ::io_uring_get_sqe(&ring_);
      if (!sqe)
        break;
      const auto ack_key = ++next_key_;
      if (!ack_key || ack_key >= wake_key_)
        std::terminate();
      const auto original_token = original->second.token;
      try {
        auto [ack, inserted] = entries_.try_emplace(ack_key);
        ack->second.token = original_token;
        ack->second.ack = true;
      } catch (...) {
        std::terminate();
      }
      ::io_uring_prep_cancel64(sqe, original_key, 0);
      ::io_uring_sqe_set_data64(sqe, ack_key);
      pending_cancels_.pop_front();
    }
  }

  /**
   *  @brief 唯一 CQ 消费者在 SQ 锁内按预算解码真实 CQE，不直接调用协程消费者。
   * @param output 中立事件输出预算；尚未读取的普通 CQE 留在内核完成队列。
   * @return 本轮业务结果/ACK/释放事件数；控制 wake/TIMEOUT 不计入用户结果。
   */
  int harvest(std::span<backend_event> output) noexcept {
    std::size_t count = 0;
    std::array<io_uring_cqe*, 256> pending;
    const auto available = ::io_uring_peek_batch_cqe(
        &ring_, pending.data(), static_cast<unsigned>(std::min(output.size(), pending.size())));
    for (unsigned index = 0; index < available; ++index) {
      auto* cqe = pending[index];

      const auto key = ::io_uring_cqe_get_data64(cqe);
      if (key == wake_key_) {
        wake_armed_ = false;
        control_progress_ = true;
        std::uint64_t ignored;
#if defined(FAIO_URING_CONTROL_TESTS) && FAIO_URING_CONTROL_TESTS
        control_test_step(control_test_point::HARVEST_BEFORE_DRAIN);
#endif
        while (::read(wake_fd_, &ignored, sizeof(ignored)) > 0) {
        }
#if defined(FAIO_URING_CONTROL_TESTS) && FAIO_URING_CONTROL_TESTS
        control_test_step(control_test_point::HARVEST_AFTER_DRAIN);
#endif
      }
      const auto found = entries_.find(key);
      if (found != entries_.end()
          && (found->second.control_timeout || (found->second.ack && !found->second.token))) {
        if (found->second.control_timeout && timer_key_ == key)
          timer_key_ = 0;
        control_progress_ = true;
        entries_.erase(found);
      } else if (found != entries_.end()) {
        auto& value = found->second;
        auto kind = value.ack ? backend_event_kind::CANCEL_ACK : backend_event_kind::RESULT;
        if (value.ack)
          ++statistics_.cancel_ack;
        else if (cqe->flags & IORING_CQE_F_NOTIF) {
          kind = backend_event_kind::BUFFER_RELEASE;
          ++statistics_.buffer_notifications;
        } else
          ++statistics_.native_completed;
        if (value.request && value.request->kind == operation_kind::RECVFROM && cqe->res >= 0
            && value.request->output_address_length)
          *value.request->output_address_length = value.request->message.msg_namelen;
        if (value.request && value.request->kind == operation_kind::RECVMSG && cqe->res >= 0) {
          auto& r = *value.request;
          r.output_message->msg_namelen = r.message.msg_namelen;
          r.output_message->msg_controllen = r.message.msg_controllen;
          r.output_message->msg_flags = r.message.msg_flags;
        }
        const bool more = !value.ack && (cqe->flags & IORING_CQE_F_MORE);
        output[count++] = {kind, value.token, cqe->res, more ? 1u : 0u};
        if (!more) {
          if (value.sigpipe_sensitive)
            --sigpipe_operations_;
          if (value.request)
            value.request->native_completion_key = 0;
          entries_.erase(found);
        }
      }
    }
    ::io_uring_cq_advance(&ring_, available);
    return static_cast<int>(count);
  }
#if defined(FAIO_URING_CONTROL_TESTS) && FAIO_URING_CONTROL_TESTS
  /**
   * @brief 仅 tests/faio_uring_control_tests 的唯一 TU 启用；不与其他配置的 TU
   * 链接，避免 ODR 分歧。
   */
  friend struct uring_control_test_access;
  using control_test_point = uring_control_test_point;

  /** @brief 在测试构建启用的确定性竞态位置调用测试钩子。 */
  void control_test_step(control_test_point point) const noexcept { control_test_.step(point); }

  /** @brief 经可注入测试入口提交原生队列；普通构建直接使用系统接口。 */
  int control_test_submit() noexcept {
    return control_test_ ? control_test_.submit(&ring_) : ::io_uring_submit(&ring_);
  }

  basic_control_policy_ref<uring_control_policy> control_test_;

#endif
  static constexpr std::uint64_t wake_key_ =
      UINT64_MAX; /**< 唯一控制 wake 键，普通键禁止回绕到此值。 */
  mutable std::atomic_bool wake_pending_{
      false}; /**< 通知交接独立于 eventfd drain；const wake 可跨线程。 */
  bool wake_armed_{}, control_progress_{}; /**< 本轮是否处理了控制事件，避免误判后端完全无进展。 */
  std::uint64_t timer_key_{}; /**< 旧内核原生 TIMEOUT 的唯一在途身份，最终 CQE 后归零。 */
  io_uring ring_{};           /**< 私有 SQ/CQ 内核映射，只有本后端拥有退出责任。 */
  int wake_fd_{-1};           /**< 仅用于控制通知，不接管业务 fd 或借用业务 buffer。 */
  bool initialized_{};        /**< 原生后端是否已成功初始化，析构据此回收资源。 */
  bool extended_wait_{}; /**< 实际探测到的 EXT_ARG 能力；false 也用于测试原生 TIMEOUT 兼容路径。 */
  bool socket_commands_{};            /**< 是否探测到所需 socket 命令支持，运行时按缓存分派。 */
  bool accept_nowait_{};              /**< 是否支持原生非等待 accept 扩展。 */
  std::array<bool, 256> supported_{}; /**< 按原生操作码缓存的能力表，热路径无需重复探测。 */
  mutable std::mutex mutex_;          /**< 串行保护 SQ/记录/取消；GETEVENTS 睡眠不持有此锁。 */
  std::uint64_t next_key_{};          /**< 单调完成键，避免迟到 CQE 的 ABA；耗尽时禁止复用。 */
  std::pmr::unsynchronized_pool_resource
      entry_memory_; /**< 在 entries_ 之前声明，保证先销毁所有节点和 bucket，再释放其内存。SQ、CQ 和取消均由 mutex_ 串行保护，私有池无需额外锁或辅助线程。节点按实际在途高水位复用，稳态下无需逐条 malloc/free。 */
  std::pmr::unordered_map<std::uint64_t, entry> entries_{
      &entry_memory_}; /**< 地址稳定的原请求/ACK/TIMEOUT 节点，仍以不复用整数键查找。 */
  std::deque<std::uint64_t> pending_cancels_; /**< 尚未准备到 SQ 的取消责任，背压时保留。 */
  backend_statistics statistics_;             /**< 后端提交、完成及控制事件的诊断统计。 */
  std::size_t
      sigpipe_operations_{}; /**< 尚未消费最终 CQE 的敏感 WRITE 数，决定是否保护原生调用窗口中的信号。 */
};
}  // namespace faio::io::detail
#endif
