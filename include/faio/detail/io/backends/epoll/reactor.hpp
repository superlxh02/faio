#pragma once
#if defined(__linux__)
#include "faio/detail/io/reactor/reactor_ref.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <system_error>
#include <unistd.h>

namespace faio::io::detail {
/// @brief Linux epoll ET 后端；注册在整个资源生命周期内保持稳定。
/// @details 每次操作先执行 syscall，只有 EAGAIN 才等待，因此短读不会丢边沿。
class epoll_reactor {
public:
  static constexpr const char *backend_name = "epoll";
  /** @brief 建立拥有型内核队列与独立控制通道，失败时回收已成功构造的句柄。
   * @throws std::system_error 创建队列、设置继承标志或注册控制事件失败。
   */
  epoll_reactor() {
    poll_fd_ =
        ::epoll_create1(EPOLL_CLOEXEC); // 原子设置 CLOEXEC，避免 exec 泄漏。
    if (poll_fd_ < 0)
      throw std::system_error(errno, std::generic_category(), "epoll_create1");
    wake_fd_ =
        ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC); // 计数唤醒不丢失突发通知。
    if (wake_fd_ < 0) {
      // 保存 eventfd 创建错误，再关闭已经拥有的 epoll，不重试可能已关闭的 fd。
      const int e = errno;
      ::close(poll_fd_);
      throw std::system_error(e, std::generic_category(), "eventfd");
    }
    // 内核事件结构清零，只开放控制读边沿并保留零完成键。
    epoll_event event{};
    // eventfd 采用 ET，消费方必须排空计数，才能继续接收下一次控制通知。
    event.events = EPOLLIN | EPOLLET;
    event.data.u64 = 0; // 零 token 专用于控制唤醒。
    if (::epoll_ctl(poll_fd_, EPOLL_CTL_ADD, wake_fd_, &event) < 0) {
      // 控制注册失败尚无业务引用，按构造逆序释放 eventfd 与 epoll。
      const int e = errno;
      ::close(wake_fd_);
      ::close(poll_fd_);
      throw std::system_error(e, std::generic_category(),
                              "epoll wake registration");
    }
  }
  epoll_reactor(const epoll_reactor &) = delete;
  epoll_reactor &operator=(const epoll_reactor &) = delete;
  ~epoll_reactor() {
    ::close(wake_fd_);
    ::close(poll_fd_);
  } // 描述符只关闭一次，不重试 EINTR。
  /// @brief 双方向 ET 注册保持到资源关闭；可读、可写请求由核心分别拥有执行权。
  /// @return 零或负 errno；失败不改变 resource 的 registered 标记。
  /// @param fd 已由域保证非阻塞的业务描述符，本函数不接管其关闭责任。
  /// @param key 单调资源代际，内核事件只返回该数值，不保存协程地址。
  int attach(int fd, std::uint64_t key) noexcept {
    // 一次初始化持久双方向注册，不在每个 read/write 后 MOD 或重建 key。
    epoll_event event{};
    event.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP |
                   EPOLLET; // 永久 ET 双方向消除逐次 MOD/rearm。
    event.data.u64 = key;   // 单调资源代际 token 排除 fd 重用后的迟到事件。
    // 注册失败返回负 errno；由域决定是否接受操作，不在此恢复协程。
    return ::epoll_ctl(poll_fd_, EPOLL_CTL_ADD, fd, &event) == 0 ? 0 : -errno;
  }
  /// @brief 在原生 fd 关闭/导出前注销；迟到事件仍由资源 key 校验拒绝。
  void detach(int fd) noexcept {
    // 注销控制面允许原 fd 已失效；迟到事件仍按不复用的资源代际拒绝。
    (void)::epoll_ctl(poll_fd_, EPOLL_CTL_DEL, fd, nullptr);
  }
  /// @brief 将 epoll 位映射为平台中立提示，EOF 不替代真实 recv 返回值。
  /// @param output 一次事件输出预算，未消费的事件留给后续 driver。
  /// @param timeout nullopt 无限等待、零非阻塞，单位毫秒。
  /// @return 事件数或负 errno；EINTR 交宿主重新计算绝对截止时间。
  int poll(std::span<readiness_event> output,
           std::optional<int> timeout) noexcept {
    if (output.empty())
      return 0; // 空批次是非阻塞 no-op，不能向 epoll_wait 传 maxevents=0。
    std::array<epoll_event, 256>
        events; // epoll_wait 完整写出返回数量内的结构；未取出的事件留在内核。
    // 固定栈容量与调用方预算取较小值，避免丢弃本轮取出的事件。
    const int count =
        ::epoll_wait(poll_fd_, events.data(),
                     static_cast<int>(std::min(output.size(), events.size())),
                     // -1 是内核无限等待；eventfd 或真实资源事件均可打断。
                     timeout.value_or(-1));
    if (count < 0)
      return errno == EINTR
                 ? 0
                 : -errno; // 中断交回宿主重算 deadline，避免延长等待。
    for (int i = 0; i < count; ++i) {
      // 索引限于 epoll_wait 真正写出的数量，不读取未初始化的剩余槽。
      const auto &event = events[static_cast<std::size_t>(i)];
      if (event.data.u64 == 0) {
        // 控制通道只消费 eventfd 计数，不读取任何业务 socket 数据。
        std::uint64_t value{};
        for (;;) {
          const auto drained = ::read(wake_fd_, &value, sizeof(value));
          if (drained > 0 || (drained < 0 && errno == EINTR))
            continue; // 中断不能留下未排空 ET 唤醒。
          break;      // EAGAIN 表示计数器真正排空，下一次控制发布可以再次触发。
        }
      }
      std::uint32_t
          flags{}; // 分别保留可读、可写、错误及半关闭信息，不互相覆盖。
      if (event.events & EPOLLIN)
        // 可读提示只授权读方向重试，是否有数据/EOF 仍由真实 read 判断。
        flags |= readable_bit;
      if (event.events & EPOLLOUT)
        // 可写提示只授权写方向重试，允许之后的真实 send 仍返回 EAGAIN。
        flags |= writable_bit;
      if (event.events & EPOLLERR)
        // 错误同时通知任何方向，由对应真实 syscall 取得最终错误。
        flags |= error_bit;
      if (event.events & EPOLLHUP)
        flags |= readable_bit | writable_bit |
                 closed_bit; // 全关闭使两方向都可执行真实 IO 判断终态。
      if (event.events & EPOLLRDHUP)
        flags |=
            readable_bit |
            read_closed_bit; // 对端写半关闭只提示读 EOF，不能假定本端不可写。
      // 保留完整资源 key 和独立方向/关闭位，控制 key=0 不命中业务资源。
      output[static_cast<std::size_t>(i)] = {event.data.u64, flags};
    }
    return count;
  }
  /// @brief eventfd 非阻塞计数通知；已饱和时表示内核中已经存在唤醒责任。
  void wake() noexcept {
    // eventfd 每次累加一个通知，多个发布者合并时仍保持可读。
    const std::uint64_t value = 1;
    ssize_t result;
    do {
      // 非阻塞写不会等待 driver；EINTR 重试不会丢失本次尚未写入的计数。
      result = ::write(wake_fd_, &value, sizeof(value));
    } while (result < 0 && errno == EINTR);
    // EAGAIN 表示计数器已满，即内核已有待处理唤醒，不需要阻塞生产者。
  }

private:
  int poll_fd_{-1}; ///< 拥有 epoll 实例。
  int wake_fd_{-1}; ///< 独立控制通道，不占用普通操作容量。
};
/// @brief 唯一拥有具体 reactor 的类型擦除盒，析构通过匹配函数表回收内核句柄。
inline reactor_box make_platform_reactor() {
  return reactor_box{std::make_unique<epoll_reactor>()};
}
} // namespace faio::io::detail
#endif
