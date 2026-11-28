/**
 * @file faio/detail/io/backend/epoll/reactor.hpp
 * @brief epoll 就绪后端：就绪反应器。
 */
#pragma once
#if defined(__linux__)
#include "faio/detail/io/protocol/reactor.hpp"
#if defined(FAIO_REACTOR_CONTROL_TESTS) && FAIO_REACTOR_CONTROL_TESTS
#include "faio/detail/io/testing/reactor_control_policy.hpp"
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
/**
 * @brief Linux epoll ET 后端；注册在整个资源生命周期内保持稳定。
 * @details 每次操作先执行 syscall，只有 EAGAIN 才等待，因此短读不会丢边沿。
 */
class epoll_reactor {
 public:
  static constexpr const char* backend_name = "epoll"; /**< 后端名称常量，用于能力报告与诊断。 */

  /**
   *  @brief 建立自行管理的内核队列与独立控制通道，失败时回收已成功构造的句柄。
   * @throws std::system_error 创建队列、设置继承标志或注册控制事件失败。
   */
  epoll_reactor() {
    poll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (poll_fd_ < 0)
      throw std::system_error(errno, std::generic_category(), "epoll_create1");
    wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd_ < 0) {
      const int e = errno;
      ::close(poll_fd_);
      throw std::system_error(e, std::generic_category(), "eventfd");
    }
    epoll_event event{};
    event.events = EPOLLIN | EPOLLET;
    event.data.u64 = 0;
    if (::epoll_ctl(poll_fd_, EPOLL_CTL_ADD, wake_fd_, &event) < 0) {
      const int e = errno;
      ::close(wake_fd_);
      ::close(poll_fd_);
      throw std::system_error(e, std::generic_category(), "epoll wake registration");
    }
  }

  /** @brief 禁止复制构造 Linux epoll 就绪反应器。 */
  epoll_reactor(const epoll_reactor&) = delete;

  /** @brief 禁止复制赋值。 */
  epoll_reactor& operator=(const epoll_reactor&) = delete;

  /** @brief 销毁 Linux epoll 就绪反应器。 */
  ~epoll_reactor() {
    ::close(wake_fd_);
    ::close(poll_fd_);
  }

  /**
   * @brief 双方向 ET 注册保持到资源关闭；可读、可写请求由核心分别拥有执行权。
   * @return 零或负 errno；失败不改变 resource 的 registered 标记。
   * @param fd 已由域保证非阻塞的业务描述符，本函数不接管其关闭责任。
   * @param key 单调资源代际，内核事件只返回该数值，不保存协程地址。
   */
  int register_resource(int fd, std::uint64_t key) noexcept {
    epoll_event event{};
    event.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
    event.data.u64 = key;
    return ::epoll_ctl(poll_fd_, EPOLL_CTL_ADD, fd, &event) == 0 ? 0 : -errno;
  }

  /** @brief 在原生 fd 关闭/导出前注销；迟到事件仍由资源 key 校验拒绝。 */
  void unregister_resource(int fd) noexcept {
    (void)::epoll_ctl(poll_fd_, EPOLL_CTL_DEL, fd, nullptr);
  }

  /**
   * @brief 将 epoll 位映射为平台中立提示，EOF 不替代真实 recv 返回值。
   * @param output 一次事件输出预算，未消费的事件留给后续 driver。
   * @param timeout nullopt 无限等待、零非阻塞，单位毫秒。
   * @return 事件数或负 errno；EINTR 交宿主重新计算绝对截止时间。
   */
  int poll(std::span<readiness_event> output, std::optional<int> timeout) noexcept {
    return poll_into(output, timeout, [](std::uint64_t key, std::uint32_t flags) noexcept {
      return readiness_event{key, flags};
    });
  }

  /** @brief 解析一次原生事件，直接写入调用方要求的事件格式。 */
  template <class Event, class Map>
    requires std::is_nothrow_invocable_r_v<Event, Map&, std::uint64_t, std::uint32_t>
  int poll_into(std::span<Event> output, std::optional<int> timeout, Map map) noexcept {
    if (output.empty())
      return 0;
    const bool noticed = wake_pending_.exchange(false, std::memory_order_seq_cst);
#if defined(FAIO_REACTOR_CONTROL_TESTS) && FAIO_REACTOR_CONTROL_TESTS
    control_test_step(reactor_control_test_point::POLL_AFTER_CONSUME);
#endif
    if (noticed)
      timeout = 0;
    std::array<epoll_event, 256> events;
    const int count = ::epoll_wait(poll_fd_,
                                   events.data(),
                                   static_cast<int>(std::min(output.size(), events.size())),
                                   timeout.value_or(-1));
    if (count < 0)
      return errno == EINTR ? 0 : -errno;
    for (int i = 0; i < count; ++i) {
      const auto& event = events[static_cast<std::size_t>(i)];
      if (event.data.u64 == 0) {
        std::uint64_t value{};
        for (;;) {
          const auto drained = ::read(wake_fd_, &value, sizeof(value));
          if (drained < 0 && errno == EINTR)
            continue;
          break;
        }
      }
      std::uint32_t flags{};
      if (event.events & EPOLLIN)
        flags |= READABLE_BIT;
      if (event.events & EPOLLOUT)
        flags |= WRITABLE_BIT;
      if (event.events & EPOLLERR)
        flags |= ERROR_BIT;
      if (event.events & EPOLLHUP)
        flags |= READABLE_BIT | WRITABLE_BIT | CLOSED_BIT;
      if (event.events & EPOLLRDHUP)
        flags |= READABLE_BIT | READ_CLOSED_BIT;
      output[static_cast<std::size_t>(i)] = map(event.data.u64, flags);
    }
    return count;
  }

  /** @brief 合并同一 poll 边界前的通知，只由首个发布者写入 eventfd。 */
  void wake() noexcept {
    if (wake_pending_.exchange(true, std::memory_order_seq_cst))
      return;
#if defined(FAIO_REACTOR_CONTROL_TESTS) && FAIO_REACTOR_CONTROL_TESTS
    control_test_step(reactor_control_test_point::WAKE_BEFORE_SYSCALL);
#endif
    const std::uint64_t value = 1;
    ssize_t result;
    do {
      result = ::write(wake_fd_, &value, sizeof(value));
    } while (result < 0 && errno == EINTR);
  }

 private:
#if defined(FAIO_REACTOR_CONTROL_TESTS) && FAIO_REACTOR_CONTROL_TESTS
  friend struct reactor_control_test_access;

  /** @brief 在测试构建启用的确定性竞态位置调用测试钩子。 */
  void control_test_step(reactor_control_test_point point) const noexcept {
    control_test_.step(point);
  }

  basic_control_policy_ref<reactor_control_policy> control_test_;

#endif
  std::atomic<bool> wake_pending_{false}; /**< 仅由下次非空 poll 消费，独立于内核事件排空。 */
  int poll_fd_{-1};                       /**< 拥有 epoll 实例。 */
  int wake_fd_{-1};                       /**< 独立控制通道，不占用普通操作容量。 */
};

}  // namespace faio::io::detail
#endif
