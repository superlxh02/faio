#pragma once
#include "faio/detail/io/backend_protocol.hpp"
#include "faio/detail/io/reactor/reactor_ref.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
namespace faio::io::detail {
/** @brief 将 reactor 接入同一完成协议，syscall attempt 属于统一 domain。
 * @details 此适配器只产生 readiness；core 执行非阻塞 syscall 并形成结果事件。
 *          它没有内核 payload 引用，文件阻塞 provider 与控制取消也在 core
 * 统一排空。
 */
class readiness_adapter {
public:
  static constexpr bool native_proactor = false;
  static constexpr const char *backend_name = "readiness";
  /// @brief 只移动接管现有 reactor，不为每个 IO 请求增设包装、任务或线程。
  explicit readiness_adapter(reactor_box reactor)
      : reactor_(std::move(reactor)) {}
  /// @brief 控制面转发持久资源注册；fd 关闭责任与资源代际仍由域拥有。
  int attach(int fd, std::uint64_t key) noexcept {
    return reactor_.attach(fd, key);
  }
  /// @brief 转发注销，迟到 readiness 由域核对单调资源 key。
  void detach(int fd) noexcept { reactor_.detach(fd); }
  /// @brief 明确拒绝原生 typed 提交，reactor 的 syscall attempt 只在域内执行。
  backend_submit_result try_submit(backend_operation) noexcept {
    return {backend_submit_status::rejected, EOPNOTSUPP};
  } // 不把 reactor 伪装成原生 Proactor。
  /// @brief adapter 没有 SQ 或原生提交责任；结果由 reactor poll 和域 attempt
  /// 推进。
  backend_flush_result flush() noexcept { return {}; }
  /// @brief 一次有界就绪批次仅改事件形状，不生成虚假 IO 字节结果。
  int poll(std::span<backend_event> out, std::optional<int> ms) noexcept {
    std::array<readiness_event, 256> events; // 仅读取 reactor 完整写入的 count
                                             // 个输出槽，避免空 poll 清零整批。
    const int n = reactor_.poll(
        std::span(events).first(std::min(out.size(), events.size())),
        ms); // 限制驱动预算，剩余事件保留在内核。
    for (int i = 0; i < n; ++i) {
      const auto index =
          static_cast<std::size_t>(i); // 只访问 reactor 已输出的有效事件。
      out[index] = {
          backend_event_kind::readiness, events[index].key, 0,
          events[index].flags}; // 只传资源代际，不传播 fd 地址或协程指针。
    }
    return n; // 负错误保留给 domain，不能误当作零个正常事件。
  }
  void request_cancel(std::uint64_t) noexcept {
  } // 无内核 payload 引用，core 取消后即可建立终态。
  /// @brief 直接通知底层控制通道，实际请求责任不依赖通知次数。
  void wake() noexcept { reactor_.wake(); }
  /// @brief 无原生请求需要后端取消；域停机负责拒绝新 IO 并排空文件辅助服务。
  void begin_shutdown() noexcept {}
  /// @brief reactor 不持有业务 payload；true 不表示整个域或服务线程已经排空。
  bool quiescent() noexcept {
    return true;
  } // 此处只检查 backend；domain 仍等待操作/文件服务/清理服务。
  /// @brief 不宣称支持任何 native opcode，避免接入方跳过真实 syscall attempt。
  bool supports(std::uint32_t) noexcept { return false; }
  const char *reactor_name() const noexcept { return reactor_.name(); }
  const char *name() const noexcept { return reactor_.name(); }
  backend_statistics statistics() const noexcept { return {}; }

private:
  reactor_box
      reactor_; ///< 唯一拥有实际内核就绪队列，生命周期覆盖全部驱动调用。
};
} // namespace faio::io::detail
