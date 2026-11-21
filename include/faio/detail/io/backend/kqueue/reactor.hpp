/**
 * @file faio/detail/io/backend/kqueue/reactor.hpp
 * @brief kqueue 就绪后端：就绪反应器。
 */
#pragma once
#if defined(__APPLE__) || defined(__FreeBSD__)
#include "faio/detail/io/protocol/reactor.hpp"
#if defined(FAIO_REACTOR_CONTROL_TESTS) && FAIO_REACTOR_CONTROL_TESTS
#include "faio/detail/io/testing/reactor_control_policy.hpp"
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
/** @brief kqueue 的独立读/写过滤器；EV_CLEAR 配合先尝试 syscall 的协议使用。 */
class kqueue_reactor {
 public:
  static constexpr const char* backend_name = "kqueue"; /**< 后端名称常量，用于能力报告与诊断。 */

  /**
   *  @brief 建立自行管理的内核队列与独立控制通道，失败时回收已成功构造的句柄。
   * @throws std::system_error 创建队列、设置继承标志或注册控制事件失败。
   */
  kqueue_reactor() {
    poll_fd_ = ::kqueue();
    if (poll_fd_ < 0)
      throw std::system_error(errno, std::generic_category(), "kqueue");
    if (::fcntl(poll_fd_, F_SETFD, FD_CLOEXEC) < 0) {
      const int e = errno;
      ::close(poll_fd_);
      throw std::system_error(e, std::generic_category(), "kqueue CLOEXEC");
    }
    struct kevent change{};
    EV_SET(&change, 0, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (::kevent(poll_fd_, &change, 1, nullptr, 0, nullptr) < 0) {
      const int e = errno;
      ::close(poll_fd_);
      throw std::system_error(e, std::generic_category(), "kqueue user event");
    }
  }

  /** @brief 禁止复制构造 BSD kqueue 就绪反应器。 */
  kqueue_reactor(const kqueue_reactor&) = delete;

  /** @brief 禁止复制赋值。 */
  kqueue_reactor& operator=(const kqueue_reactor&) = delete;

  /** @brief 域停止 driver 后一次关闭队列；不重试 fd 可能已经释放的 close。 */
  ~kqueue_reactor() { ::close(poll_fd_); }

  /**
   * @brief 一次提交两个过滤器并逐一检查 EV_RECEIPT，避免半注册资源。
   * @param fd 已由域保证非阻塞的业务描述符，本函数不接管其关闭责任。
   * @param key 单调资源代际，内核事件只返回该数值，不保存协程地址。
   */
  int register_resource(int fd, std::uint64_t key) noexcept {
    std::array<struct kevent, 2> changes{}, receipts{};
    auto* token = reinterpret_cast<void*>(static_cast<std::uintptr_t>(key));
    EV_SET(&changes[0],
           static_cast<uintptr_t>(fd),
           EVFILT_READ,
           EV_ADD | EV_CLEAR | EV_RECEIPT,
           0,
           0,
           token);
    EV_SET(&changes[1],
           static_cast<uintptr_t>(fd),
           EVFILT_WRITE,
           EV_ADD | EV_CLEAR | EV_RECEIPT,
           0,
           0,
           token);
    const int count = ::kevent(poll_fd_, changes.data(), 2, receipts.data(), 2, nullptr);
    if (count < 0)
      return -errno;
    for (int i = 0; i < count; ++i)
      if (receipts[static_cast<std::size_t>(i)].data != 0) {
        const int error = static_cast<int>(receipts[static_cast<std::size_t>(i)].data);
        unregister_resource(fd);
        return -error;
      }
    return 0;
  }

  /** @brief 每个 EV_DELETE 独立取得回执，允许已关闭的方向返回 ENOENT。 */
  void unregister_resource(int fd) noexcept {
    std::array<struct kevent, 2> changes{}, receipts{};
    EV_SET(&changes[0],
           static_cast<uintptr_t>(fd),
           EVFILT_READ,
           EV_DELETE | EV_RECEIPT,
           0,
           0,
           nullptr);
    EV_SET(&changes[1],
           static_cast<uintptr_t>(fd),
           EVFILT_WRITE,
           EV_DELETE | EV_RECEIPT,
           0,
           0,
           nullptr);
    const timespec zero{};
    (void)::kevent(poll_fd_, changes.data(), 2, receipts.data(), 2, &zero);
  }

  /**
   * @brief 使用调用方预算读取批次，将过滤器事件映射为统一 readiness 位。
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
    control_test_step(reactor_control_test_point::poll_after_consume);
#endif
    if (noticed)
      timeout = 0;
    std::array<struct kevent, 256> events;
    timespec duration{};
    const timespec* wait = nullptr;
    if (timeout) {
      duration.tv_sec = *timeout / 1000;
      duration.tv_nsec = (*timeout % 1000) * 1000000L;
      wait = &duration;
    }
    const int count = ::kevent(poll_fd_,
                               nullptr,
                               0,
                               events.data(),
                               static_cast<int>(std::min(output.size(), events.size())),
                               wait);
    if (count < 0)
      return errno == EINTR ? 0 : -errno;
    for (int i = 0; i < count; ++i) {
      const auto& event = events[static_cast<std::size_t>(i)];
      std::uint32_t flags{};
      if (event.filter == EVFILT_READ)
        flags |= readable_bit;
      if (event.filter == EVFILT_WRITE)
        flags |= writable_bit;
      if (event.flags & EV_ERROR)
        flags |= error_bit;
      if (event.flags & EV_EOF) {
        if (event.filter == EVFILT_READ)
          flags |= readable_bit | read_closed_bit;
        else if (event.filter == EVFILT_WRITE)
          flags |= writable_bit | write_closed_bit;
      }
      output[static_cast<std::size_t>(i)] =
          map(event.filter == EVFILT_USER
                  ? 0
                  : static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(event.udata)),
              flags);
    }
    return count;
  }

  /** @brief NOTE_TRIGGER 合并控制唤醒，不消耗 socket 数据或普通操作槽。 */
  void wake() noexcept {
    if (wake_pending_.exchange(true, std::memory_order_seq_cst))
      return;
#if defined(FAIO_REACTOR_CONTROL_TESTS) && FAIO_REACTOR_CONTROL_TESTS
    control_test_step(reactor_control_test_point::wake_before_syscall);
#endif
    struct kevent change{};
    EV_SET(&change, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    int result;
    do {
      result = ::kevent(poll_fd_, &change, 1, nullptr, 0, nullptr);
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
  int poll_fd_{-1};                       /**< 拥有内核队列；生命周期覆盖全部 driver 调用。 */
};

}  // namespace faio::io::detail
#endif
