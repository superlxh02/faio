/**
 * @file faio/detail/io/backend/epoll/reactor.hpp
 * @brief 跨平台异步 IO：epoll 就绪后端：就绪反应器。
 * @details 统一后端只通过资源键或操作代际返回事件；控制回执与业务完成分别处理。内核解除请求与负载引用后才允许复用稳定存储，唤醒使用独立控制通道。
 */
#pragma once
#if defined(__linux__)
#include "faio/detail/io/protocol/reactor.hpp"
#if defined(FAIO_REACTOR_CONTROL_TESTS) && FAIO_REACTOR_CONTROL_TESTS
#include "faio/detail/io/base/reactor.hpp"
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <type_traits>
#include <cerrno>
#include <optional>
#include <span>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <system_error>
#include <unistd.h>

namespace faio::io::detail {
/// @brief Linux epoll ET 后端；注册在整个资源生命周期内保持稳定。
/// @details 每次操作先执行 syscall，只有 EAGAIN 才等待，因此短读不会丢边沿。
class epoll_reactor {
 public:
  static constexpr const char* backend_name = "epoll";  ///< 后端名称常量，用于能力报告与诊断。

  /** @brief 建立拥有型内核队列与独立控制通道，失败时回收已成功构造的句柄。
   * @throws std::system_error 创建队列、设置继承标志或注册控制事件失败。
   */
  epoll_reactor() {
    /// @brief 原子设置 CLOEXEC，避免 exec 泄漏。
    poll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (poll_fd_ < 0)
      throw std::system_error(errno, std::generic_category(), "epoll_create1");
    /// @brief 计数唤醒不丢失突发通知。
    wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd_ < 0) {
      /// @brief 保存 eventfd 创建错误，再关闭已经拥有的 epoll，不重试可能已关闭的 fd。
      const int e = errno;
      ::close(poll_fd_);
      throw std::system_error(e, std::generic_category(), "eventfd");
    }
    /// @brief 内核事件结构清零，只开放控制读边沿并保留零完成键。
    epoll_event event{};
    /// @brief eventfd 采用 ET，消费方必须排空计数，才能继续接收下一次控制通知。
    event.events = EPOLLIN | EPOLLET;
    /// @brief 零 token 专用于控制唤醒。
    event.data.u64 = 0;
    /// @brief 修改持久资源或控制描述符登记，事件只保存单调键，不保存等待器地址。
    if (::epoll_ctl(poll_fd_, EPOLL_CTL_ADD, wake_fd_, &event) < 0) {
      /// @brief 控制注册失败尚无业务引用，按构造逆序释放 eventfd 与 epoll。
      const int e = errno;
      ::close(wake_fd_);
      ::close(poll_fd_);
      throw std::system_error(e, std::generic_category(), "epoll wake registration");
    }
  }

  epoll_reactor(const epoll_reactor&) = delete;

  epoll_reactor& operator=(const epoll_reactor&) = delete;

  ~epoll_reactor() {
    ::close(wake_fd_);
    ::close(poll_fd_);
  /// @brief 描述符只关闭一次，不重试 EINTR。
  }

  /// @brief 双方向 ET 注册保持到资源关闭；可读、可写请求由核心分别拥有执行权。
  /// @return 零或负 errno；失败不改变 resource 的 registered 标记。
  /// @param fd 已由域保证非阻塞的业务描述符，本函数不接管其关闭责任。
  /// @param key 单调资源代际，内核事件只返回该数值，不保存协程地址。
  int register_resource(int fd, std::uint64_t key) noexcept {
    /// @brief 一次初始化持久双方向注册，不在每个 read/write 后 MOD 或重建 key。
    epoll_event event{};
    /// @brief 永久 ET 双方向消除逐次 MOD/rearm。
    event.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
    /// @brief 单调资源代际 token 排除 fd 重用后的迟到事件。
    event.data.u64 = key;
    /// @brief 注册失败返回负 errno；由域决定是否接受操作，不在此恢复协程。
    return ::epoll_ctl(poll_fd_, EPOLL_CTL_ADD, fd, &event) == 0 ? 0 : -errno;
  }

  /// @brief 在原生 fd 关闭/导出前注销；迟到事件仍由资源 key 校验拒绝。
  void unregister_resource(int fd) noexcept {
    /// @brief 注销控制面允许原 fd 已失效；迟到事件仍按不复用的资源代际拒绝。
    (void)::epoll_ctl(poll_fd_, EPOLL_CTL_DEL, fd, nullptr);
  }

  /// @brief 将 epoll 位映射为平台中立提示，EOF 不替代真实 recv 返回值。
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
    /// @brief 空批次是非阻塞 no-op，不能向 epoll_wait 传 maxevents=0。
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
    /// @brief epoll_wait 完整写出返回数量内的结构；未取出的事件留在内核。
    std::array<epoll_event, 256>
        events;
    /// @brief 固定栈容量与调用方预算取较小值，避免丢弃本轮取出的事件。
    const int count = ::epoll_wait(poll_fd_,
                                   events.data(),
                                   static_cast<int>(std::min(output.size(), events.size())),
                                   /// @brief -1 是内核无限等待；eventfd 或真实资源事件均可打断。
                                   timeout.value_or(-1));
    /// @brief 中断交回宿主重算 deadline，避免延长等待。
    if (count < 0)
      return errno == EINTR ? 0 : -errno;
    /// @brief 只处理后端实际写入的有效事件，未使用的输出槽不读取也不清零。
    for (int i = 0; i < count; ++i) {
      /// @brief 索引限于 epoll_wait 真正写出的数量，不读取未初始化的剩余槽。
      const auto& event = events[static_cast<std::size_t>(i)];
      if (event.data.u64 == 0) {
        /// @brief 控制通道只消费 eventfd 计数，不读取任何业务 socket 数据。
        std::uint64_t value{};
        for (;;) {
          const auto drained = ::read(wake_fd_, &value, sizeof(value));
          /// @brief 中断不能留下未排空 ET 唤醒。
          if (drained > 0 || (drained < 0 && errno == EINTR))
            continue;
          /// @brief EAGAIN 表示计数器真正排空，下一次控制发布可以再次触发。
          break;
        }
      }
      /// @brief 分别保留可读、可写、错误及半关闭信息，不互相覆盖。
      std::uint32_t flags{};
      if (event.events & EPOLLIN)
        /// @brief 可读提示只授权读方向重试，是否有数据/EOF 仍由真实 read 判断。
        flags |= readable_bit;
      if (event.events & EPOLLOUT)
        /// @brief 可写提示只授权写方向重试，允许之后的真实 send 仍返回 EAGAIN。
        flags |= writable_bit;
      if (event.events & EPOLLERR)
        /// @brief 错误同时通知任何方向，由对应真实 syscall 取得最终错误。
        flags |= error_bit;
      /// @brief 全关闭使两方向都可执行真实 IO 判断终态。
      if (event.events & EPOLLHUP)
        flags |=
            readable_bit | writable_bit | closed_bit;
      /// @brief 对端写半关闭只提示读 EOF，不能假定本端不可写。
      if (event.events & EPOLLRDHUP)
        flags |= readable_bit | read_closed_bit;
      /// @brief 保留完整资源 key 和独立方向/关闭位，控制 key=0 不命中业务资源。
      output[static_cast<std::size_t>(i)] = map(event.data.u64, flags);
    }
    return count;
  }

  /// @brief 合并同一 poll 边界前的通知，只由首个发布者写入 eventfd。
  void wake() noexcept {
    if (wake_pending_.exchange(true, std::memory_order_seq_cst))
      return;
#if defined(FAIO_REACTOR_CONTROL_TESTS) && FAIO_REACTOR_CONTROL_TESTS
    control_test_step(reactor_control_test_point::wake_before_syscall);
#endif
    /// @brief 真正工作已在调用 wake 前发布；通知只是让 driver 重新检查这些状态。
    const std::uint64_t value = 1;
    ssize_t result;
    do {
      /// @brief 非阻塞写不会等待 driver；EINTR 重试不会丢失本次尚未写入的计数。
      result = ::write(wake_fd_, &value, sizeof(value));
    } while (result < 0 && errno == EINTR);
    /// @brief EAGAIN 表示计数器已满，即内核已有待处理唤醒，不需要阻塞生产者。
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
  int poll_fd_{-1};  ///< 拥有 epoll 实例。
  int wake_fd_{-1};  ///< 独立控制通道，不占用普通操作容量。
};

}
#endif
