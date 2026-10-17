/**
 * @file faio/detail/io/backend/kqueue/reactor.hpp
 * @brief 跨平台异步 IO：kqueue 就绪后端：就绪反应器。
 * @details 统一后端只通过资源键或操作代际返回事件；控制回执与业务完成分别处理。内核解除请求与负载引用后才允许复用稳定存储，唤醒使用独立控制通道。
 */
#pragma once
#if defined(__APPLE__) || defined(__FreeBSD__)
#include "faio/detail/io/protocol/reactor.hpp"
#if defined(FAIO_REACTOR_CONTROL_TESTS) && FAIO_REACTOR_CONTROL_TESTS
#include "faio/detail/io/base/reactor.hpp"
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <type_traits>
#include <cerrno>
#include <fcntl.h>
#include <optional>
#include <span>
#include <sys/event.h>
#include <system_error>
#include <unistd.h>

namespace faio::io::detail {
/// @brief kqueue 的独立读/写过滤器；EV_CLEAR 与先尝试 syscall 协议配合。
class kqueue_reactor {
 public:
  static constexpr const char* backend_name = "kqueue";  ///< 后端名称常量，用于能力报告与诊断。

  /** @brief 建立拥有型内核队列与独立控制通道，失败时回收已成功构造的句柄。
   * @throws std::system_error 创建队列、设置继承标志或注册控制事件失败。
   */
  kqueue_reactor() {
    /// @brief 唯一队列句柄在构造阶段建立，业务资源只引用注册 key。
    poll_fd_ = ::kqueue();
    /// @brief 队列创建失败没有任何控制资源需要接管。
    if (poll_fd_ < 0)
      throw std::system_error(errno, std::generic_category(), "kqueue");
    /// @brief kqueue 创建接口不带 CLOEXEC 参数，显式禁止 exec 继承内核队列。
    if (::fcntl(poll_fd_, F_SETFD, FD_CLOEXEC) < 0) {
      /// @brief 保存继承标志错误并回收队列，构造失败不会留下部分拥有状态。
      const int e = errno;
      ::close(poll_fd_);
      throw std::system_error(e, std::generic_category(), "kqueue CLOEXEC");
    }
    /// @brief 清零控制 changelist；ident=0 专属于 EVFILT_USER，不与资源代际混用。
    struct kevent change{};
    /// @brief 独立用户事件无需 pipe 读写。
    EV_SET(&change,
           0,
           EVFILT_USER,
           EV_ADD | EV_CLEAR,
           0,
           0,
           nullptr);
    /// @brief 只提交控制事件注册，不等待或消费业务事件。
    if (::kevent(poll_fd_, &change, 1, nullptr, 0, nullptr) < 0) {
      /// @brief 控制注册失败时同样一次回收队列，不把未就绪的 backend 交给 runtime。
      const int e = errno;
      ::close(poll_fd_);
      throw std::system_error(e, std::generic_category(), "kqueue user event");
    }
  }

  kqueue_reactor(const kqueue_reactor&) = delete;

  kqueue_reactor& operator=(const kqueue_reactor&) = delete;

  /// @brief 域停止 driver 后一次关闭队列；不重试 fd 可能已经释放的 close。
  ~kqueue_reactor() { ::close(poll_fd_); }

  /// @brief 一次提交两个过滤器并逐一检查 EV_RECEIPT，避免半注册资源。
  /// @param fd 已由域保证非阻塞的业务描述符，本函数不接管其关闭责任。
  /// @param key 单调资源代际，内核事件只返回该数值，不保存协程地址。
  int attach(int fd, std::uint64_t key) noexcept {
    /// @brief 变更与逐方向回执分开存储，一次失败可回滚已成功的另一过滤器。
    std::array<struct kevent, 2> changes{}, receipts{};
    /// @brief udata 是数值 token，绝不解引用。
    auto* token = reinterpret_cast<void*>(
        static_cast<std::uintptr_t>(key));
    /// @brief READ 使用 EV_CLEAR 持久边沿，EV_RECEIPT 返回该过滤器自己的注册结果。
    EV_SET(&changes[0],
           static_cast<uintptr_t>(fd),
           EVFILT_READ,
           EV_ADD | EV_CLEAR | EV_RECEIPT,
           0,
           0,
           token);
    /// @brief WRITE 独立持久注册，读/写执行权仍由域控制，过滤器不保存协程指针。
    EV_SET(&changes[1],
           static_cast<uintptr_t>(fd),
           EVFILT_WRITE,
           EV_ADD | EV_CLEAR | EV_RECEIPT,
           0,
           0,
           token);
    /// @brief 一次批量安装两方向。
    const int count = ::kevent(poll_fd_,
                               changes.data(),
                               2,
                               receipts.data(),
                               2,
                               nullptr);
    /// @brief kevent 控制调用本身失败时返回负 errno，不能当作成功安装。
    if (count < 0)
      return -errno;
    for (int i = 0; i < count; ++i)
      /// @brief EV_RECEIPT 以 data=0 表示成功；非零 data 是该变更的正 errno。
      if (receipts[static_cast<std::size_t>(i)].data != 0) {
        const int error = static_cast<int>(receipts[static_cast<std::size_t>(i)].data);
        /// @brief 回滚已经成功的另一过滤器，失败不留下半注册状态。
        detach(fd);
        return -error;
      }
    /// @brief 只有回执均成功才让域发布 registered，业务 fd 仍归资源 owner。
    return 0;
  }

  /// @brief 每个 EV_DELETE 独立取得回执，允许已关闭的方向返回 ENOENT。
  void detach(int fd) noexcept {
    /// @brief READ/WRITE 的删除各有回执，某个方向已经不存在不妨碍另一个注销。
    std::array<struct kevent, 2> changes{}, receipts{};
    /// @brief 删除读过滤器，只提交控制面变更，不把其回执当作 readable。
    EV_SET(&changes[0],
           static_cast<uintptr_t>(fd),
           EVFILT_READ,
           EV_DELETE | EV_RECEIPT,
           0,
           0,
           nullptr);
    /// @brief 写方向独立删除，注册 key 对迟到事件仍保持不可复用。
    EV_SET(&changes[1],
           static_cast<uintptr_t>(fd),
           EVFILT_WRITE,
           EV_DELETE | EV_RECEIPT,
           0,
           0,
           nullptr);
    /// @brief 零等待确保注销不阻塞 worker，EV_RECEIPT 保证不会顺便消费业务事件。
    const timespec zero{};
    /// @brief 两个过滤器都取得回执，某方向 ENOENT
    /// 不会中止另一方向的注销；不消费正常事件。
    (void)::kevent(poll_fd_, changes.data(), 2, receipts.data(), 2, &zero);
  }

  /// @brief 使用调用方预算读取批次，将过滤器事件映射为统一 readiness 位。
  /// @param output 一次事件输出预算，未消费的事件留给后续 driver。
  /// @param timeout nullopt 无限等待、零非阻塞，单位毫秒。
  /// @return 事件数或负 errno；EINTR 交宿主重新计算绝对截止时间。
  int poll(std::span<readiness_event> output, std::optional<int> timeout) noexcept {
    return poll_into(output, timeout, [](std::uint64_t key, std::uint32_t flags) noexcept {
      return readiness_event{key, flags};
    });
  }

  /// @brief 只进行一次原生事件解析，直接写入调用方的事件形状。
  template <class Event, class Map>
    requires std::is_nothrow_invocable_r_v<Event, Map&, std::uint64_t, std::uint32_t>
  int poll_into(std::span<Event> output, std::optional<int> timeout, Map map) noexcept {
    /// @brief 空输出不消费事件，也不等待。
    if (output.empty())
      return 0;
    /// @brief 在 poll 边界消费通知责任；排空内核事件不能清除此后发布的新通知。
    /// 若生产者尚未完成控制 syscall，本轮也不得进入阻塞等待。
    const bool noticed = wake_pending_.exchange(false, std::memory_order_seq_cst);
#if defined(FAIO_REACTOR_CONTROL_TESTS) && FAIO_REACTOR_CONTROL_TESTS
    control_test_step(reactor_control_test_point::poll_after_consume);
#endif
    if (noticed)
      timeout = 0;
    /// @brief 固定栈批次覆盖输出预算，未消费的内核事件保留到下一轮。
    /// @brief kevent 完整写出返回数量内的结构；只读有效输出。
    std::array<struct kevent,
               256> events;
    /// @brief 超时参数只在本次同步 kevent 期间借用，不储存在后端队列。
    timespec duration{};
    /// @brief 未提供超时时 nullptr 表示无限等待，控制 EVFILT_USER 可以跨线程打断。
    const timespec* wait = nullptr;
    if (timeout) {
      /// @brief 整秒部分；零时长仍是真正非阻塞 poll。
      duration.tv_sec = *timeout / 1000;
      /// @brief 毫秒余数转换为纳秒，值小于 1 秒。
      duration.tv_nsec = (*timeout % 1000) * 1000000L;
      /// @brief nullopt 保持 nullptr，从而允许无限等待控制唤醒。
      wait = &duration;
    }
    /// @brief 此调用没有 changelist，仅以调用方预算消费业务/控制事件。
    const int count = ::kevent(poll_fd_,
                               nullptr,
                               0,
                               events.data(),
                               static_cast<int>(std::min(output.size(), events.size())),
                               wait);
    /// @brief EINTR 返回零交宿主重算截止时间，永久错误保留负 errno。
    if (count < 0)
      return errno == EINTR ? 0 : -errno;
    /// @brief 只处理后端实际写入的有效事件，未使用的输出槽不读取也不清零。
    for (int i = 0; i < count; ++i) {
      /// @brief 只解析内核实际写出的事件；过滤器决定方向，不由 EOF 推断另一方向。
      const auto& event = events[static_cast<std::size_t>(i)];
      std::uint32_t flags{};
      if (event.filter == EVFILT_READ)
        /// @brief READ 的可读提示也覆盖其 EOF，真实读仍可先消费未读 payload。
        flags |= readable_bit;
      if (event.filter == EVFILT_WRITE)
        /// @brief WRITE 事件仅授权写方向重试，本地写半关闭不代表读方向已关闭。
        flags |= writable_bit;
      if (event.flags & EV_ERROR)
        /// @brief 内核错误保留独立位，域可以唤醒对应观察者后执行真实错误判断。
        flags |= error_bit;
      if (event.flags & EV_EOF) {
        /// @brief 本地 SHUT_WR 也可触发写 EOF，只提示写方向关闭。
        if (event.filter == EVFILT_READ)
          /// @brief 读 EOF 仍须先消费未读数据，再取得真实零读。
          flags |= readable_bit | read_closed_bit;
        else if (event.filter == EVFILT_WRITE)
          flags |= writable_bit | write_closed_bit;

      }
      /// @brief 两方向关闭信息分别保留；写 EOF 不能唤醒 readable 或伪造读 EOF。
      /// 用户控制事件还原为零 key；业务 udata 只按完整整数 token
      /// 解码，绝不解引用。
      output[static_cast<std::size_t>(i)] =
          map(event.filter == EVFILT_USER
                  ? 0
                  : static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(event.udata)),
              flags);
    }
    return count;
  }

  /// @brief NOTE_TRIGGER 合并控制唤醒，不消耗 socket 数据或普通操作槽。
  void wake() noexcept {
    if (wake_pending_.exchange(true, std::memory_order_seq_cst))
      return;
#if defined(FAIO_REACTOR_CONTROL_TESTS) && FAIO_REACTOR_CONTROL_TESTS
    control_test_step(reactor_control_test_point::wake_before_syscall);
#endif
    /// @brief 唤醒只操作独立 EVFILT_USER，不触碰业务过滤器或资源执行权。
    struct kevent change{};
    /// @brief NOTE_TRIGGER 可合并通知；真正提交/取消责任已保存在域稳定状态中。
    EV_SET(&change, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    /// @brief 保存本次控制系统调用返回值，EINTR 时按原协议重试。
    int result;
    do {
      /// @brief 向 kqueue 提交过滤器控制或领取就绪事件；回执与业务就绪由调用参数明确区分。
      result = ::kevent(poll_fd_, &change, 1, nullptr, 0, nullptr);
    } while (result < 0 && errno == EINTR);
  }

 private:
#if defined(FAIO_REACTOR_CONTROL_TESTS) && FAIO_REACTOR_CONTROL_TESTS
  friend struct reactor_control_test_access;

  /// @brief 在测试构建启用的确定性竞态位置调用测试钩子。
  void control_test_step(reactor_control_test_point point) const noexcept {
    if (control_test_step_)
      control_test_step_(point, control_test_context_);
  }

  void (*control_test_step_)(reactor_control_test_point, void*) noexcept {};  ///< 测试构建在指定竞态位置调用的静态钩子，普通构建不包含该字段。
  void* control_test_context_{};  ///< 测试钩子使用的借用上下文，覆盖钩子执行期。
#endif
  std::atomic<bool> wake_pending_{false};  ///< 仅由下次非空 poll 消费，独立于内核事件排空。
  int poll_fd_{-1};  ///< 拥有内核队列；生命周期覆盖全部 driver 调用。
};

}
#endif
