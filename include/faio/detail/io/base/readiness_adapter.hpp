/**
 * @file faio/detail/io/base/readiness_adapter.hpp
 * @brief 就绪到完成协议适配。
 */
#pragma once
#include "faio/detail/io/protocol/backend.hpp"
#include "faio/detail/io/base/reactor.hpp"
#include <algorithm>
#include <array>
#include <cerrno>

namespace faio::io::detail {
/**
 * @brief 将 reactor 接入统一完成协议；系统调用的实际尝试由共用的 domain 负责。
 * @details 适配器仅产生就绪事件，由核心执行非阻塞系统调用并生成结果事件。
 * 适配器不持有内核对负载的引用，阻塞文件 provider 和控制取消也由核心负责排空。
 */
template <class R>
class basic_readiness_adapter {
 public:
  static constexpr bool native_proactor = false; /**< 是否直接提交并接收原生异步操作完成。 */
  static constexpr bool poll_flushes_submissions =
      true; /**< readiness 没有待提交队列；poll 前无需再持有 domain 锁调用空 flush。 */
  static constexpr const char* backend_name =
      "readiness"; /**< 后端名称常量，用于能力报告与诊断。 */

  /** @brief 通过移动接管现有 reactor，无需为每个 I/O 请求增加包装、任务或线程。 */
  explicit basic_readiness_adapter(R reactor) : reactor_(std::move(reactor)) {}

  /** @brief 控制面转发持久资源注册；fd 关闭责任与资源代际仍由域拥有。 */
  int register_resource(int fd, std::uint64_t key) noexcept {
    return reactor_.register_resource(fd, key);
  }

  /** @brief 转发注销，迟到 readiness 由域核对单调资源 key。 */
  void unregister_resource(int fd) noexcept { reactor_.unregister_resource(fd); }

  /** @brief 拒绝原生类型化请求提交；reactor 的系统调用尝试仅在域内执行。 */
  backend_submit_result try_submit(backend_operation) noexcept {
    return {backend_submit_status::rejected, EOPNOTSUPP};
  }

  /**
   * @brief 适配器不维护 SQ，也不负责原生提交；结果由 reactor 轮询及域内系统调用尝试推进。
   */
  backend_flush_result flush() noexcept { return {}; }

  /** @brief 按预算读取一次就绪批次，只转换事件格式，不生成虚假 IO 字节结果。 */
  int poll(std::span<backend_event> out, std::optional<int> ms) noexcept {
    auto convert = [](std::uint64_t key, std::uint32_t flags) noexcept {
      return backend_event{backend_event_kind::readiness, key, 0, flags};
    };
    if constexpr (requires { reactor_.poll_into(out, ms, convert); }) {
      return reactor_.poll_into(out, ms, convert);
    } else {
      std::array<readiness_event, 256> events;

      const int n = reactor_.poll(std::span(events).first(std::min(out.size(), events.size())), ms);
      for (int i = 0; i < n; ++i) {
        const auto index = static_cast<std::size_t>(i);
        out[index] = {backend_event_kind::readiness, events[index].key, 0, events[index].flags};
      }
      return n;
    }
  }

  /** @brief 无内核 payload 引用，core 取消后即可建立终态。 */
  void request_cancel(std::uint64_t) noexcept {}

  /** @brief 直接通知底层控制通道，实际请求责任不依赖通知次数。 */
  void wake() noexcept { reactor_.wake(); }

  /** @brief 无原生请求需要后端取消；域停机负责拒绝新 IO 并排空文件辅助服务。 */
  void begin_shutdown() noexcept {}

  /** @brief reactor 不持有业务 payload；true 不表示整个域或服务线程已经排空。 */
  bool quiescent() noexcept { return true; }

  /** @brief 不宣称支持任何 native opcode，避免接入方跳过真实 syscall attempt。 */
  bool supports(std::uint32_t) noexcept { return false; }

  /** @brief 返回实际后端用于诊断的静态名称。 */
  const char* reactor_name() const noexcept { return reactor_.name(); }

  /** @brief 返回实际后端用于诊断的静态名称。 */
  const char* name() const noexcept { return reactor_.name(); }

  /** @brief 读取后端累计的提交、完成和控制事件统计。 */
  backend_statistics statistics() const noexcept { return {}; }

 private:
  R reactor_; /**< 唯一拥有实际内核就绪队列，生命周期覆盖全部驱动调用。 */
};
}  // namespace faio::io::detail
