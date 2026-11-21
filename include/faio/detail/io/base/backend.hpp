/**
 * @file faio/detail/io/base/backend.hpp
 * @brief 后端封装与提交协议。
 */
#pragma once
#include "faio/detail/io/traits/backend.hpp"
#include <memory>
#include <utility>

namespace faio::io::detail {
/**
 *  @brief 后端约定的 CRTP 接口；显式对象参数保留持有资源的派生对象的静态类型。
 * @details 不擦除对象类型，不保存函数表。可选能力在编译期选择，调用具体后端。
 */
template <class Derived>
class backend_api {
 protected:
  /** @brief 构造 I/O 后端协议基类。 */
  backend_api() = default;
  /** @brief 销毁 I/O 后端协议基类。 */
  ~backend_api() = default;

 public:
  /** @brief 向具体后端登记长期资源身份；不接管原生句柄的所有权。 */
  int register_resource(this Derived& self,
                        native_registration handle,
                        std::uint64_t key) noexcept {
    auto& object = self.object();
    if constexpr (requires { object.register_resource(handle, key); })
      return object.register_resource(handle, key);
    else {
      if (handle.kind != native_handle_kind::posix_descriptor || handle.value > 0x7fffffffU)
        return -22;
      return object.register_resource(static_cast<int>(handle.value), key);
    }
  }

  /** @brief 向具体后端登记长期资源身份；不接管原生句柄的所有权。 */
  int register_resource(this Derived& self, int handle, std::uint64_t key) noexcept {
    return self.register_resource(
        {static_cast<std::uintptr_t>(handle), native_handle_kind::posix_descriptor}, key);
  }

  /** @brief 撤销后端资源登记，保留句柄；上层仍须按资源身份过滤迟到事件。 */
  void unregister_resource(this Derived& self, native_registration handle) noexcept {
    auto& object = self.object();
    if constexpr (requires { object.unregister_resource(handle); })
      object.unregister_resource(handle);
    else if (handle.kind == native_handle_kind::posix_descriptor && handle.value <= 0x7fffffffU)
      object.unregister_resource(static_cast<int>(handle.value));
  }

  /** @brief 撤销后端资源登记，保留句柄；上层仍须按资源身份过滤迟到事件。 */
  void unregister_resource(this Derived& self, int handle) noexcept {
    self.unregister_resource(
        {static_cast<std::uintptr_t>(handle), native_handle_kind::posix_descriptor});
  }

  /** @brief 尝试接受稳定请求，明确区分已接受、暂时满载与拒绝。 */
  backend_submit_result try_submit(this Derived& self, backend_operation op) noexcept {
    return self.object().try_submit(op);
  }

  /** @brief 提交缓冲的写入或已准备的后端请求，失败保留尚未完成的责任。 */
  backend_flush_result flush(this Derived& self) noexcept { return self.object().flush(); }

  /** @brief 查询本后端的 poll 是否同时负责冲刷已准备的提交。 */
  bool poll_flushes_submissions(this const auto&) noexcept {
    using B = typename Derived::backend_type;
    if constexpr (requires { B::poll_flushes_submissions; })
      return static_cast<bool>(B::poll_flushes_submissions);
    else
      return false;
  }

  /** @brief 在给定输出容量与等待预算内消费一批事件，保留后续未消费事件。 */
  int poll(this Derived& self, std::span<backend_event> out, std::optional<int> timeout) noexcept {
    return self.object().poll(out, timeout);
  }

  /** @brief ACK 不代替原业务完成，后端始终收到完整代际及稳定 request。 */
  void request_cancel(this Derived& self, backend_operation op) noexcept {
    if constexpr (requires { self.object().request_cancel(op); })
      self.object().request_cancel(op);
    else
      self.object().request_cancel(op.token);
  }

  /** @brief 通过独立控制通知唤醒底层等待，处理已发布的工作。 */
  void wake(this const auto& self) noexcept { self.object().wake(); }

  /** @brief 停止接受新业务，按停止策略取消或保留已接受请求并启动排空。 */
  void begin_shutdown(this Derived& self) noexcept { self.object().begin_shutdown(); }

  /** @brief 未知 native 故障不能证明内核借用已解除，仍须 fail-fast。 */
  bool begin_failure(this Derived& self, int error) noexcept {
    if constexpr (requires { self.object().begin_failure(error); })
      return self.object().begin_failure(error);
    else {
      self.object().begin_shutdown();
      return !self.native();
    }
  }

  /** @brief 检查执行、内核引用和完成责任是否均已归还。 */
  bool quiescent(this const auto& self) noexcept { return self.object().quiescent(); }

  /** @brief 查询具体后端是否直接提供原生异步提交与完成。 */
  bool native(this const auto&) noexcept { return Derived::backend_type::native_proactor; }

  /** @brief 查询初始化时固定的原生能力，避免普通请求热路径重复探测平台。 */
  bool supports(this const auto& self, std::uint32_t kind) noexcept {
    return self.object().supports(kind);
  }

  /** @brief 返回实际后端用于诊断的静态名称。 */
  const char* name(this const auto& self) noexcept { return self.object().name(); }

  /** @brief 读取后端累计的提交、完成和控制事件统计。 */
  backend_statistics statistics(this const auto& self) noexcept {
    return self.object().statistics();
  }
};

/** @brief 原生完成后端的静态封装。 */
template <class B>
class basic_backend_box : public backend_api<basic_backend_box<B>> {
 public:
  using backend_type = B; /**< 编译期选定的具体后端类型，转发时保留静态分派。 */

  /** @brief 构造原生完成后端的静态封装。 */
  explicit basic_backend_box(std::unique_ptr<B> object) noexcept
    requires backend_implementation<B>
      : object_(std::move(object)) {}

  /** @brief 移动构造原生完成后端的静态封装。 */
  basic_backend_box(basic_backend_box&&) noexcept = default;
  /** @brief 禁止移动赋值。 */
  basic_backend_box& operator=(basic_backend_box&&) = delete;
  /** @brief 禁止复制构造原生完成后端的静态封装。 */
  basic_backend_box(const basic_backend_box&) = delete;
  /** @brief 禁止复制赋值。 */
  basic_backend_box& operator=(const basic_backend_box&) = delete;
  /** @brief 销毁原生完成后端的静态封装。 */
  ~basic_backend_box() = default;

 private:
  friend class backend_api<basic_backend_box<B>>;

  /** @brief 持有后端的 const 句柄仍允许跨线程 wake；const 不改变后端的通知状态约定。 */
  B& object() const noexcept { return *object_; }

  std::unique_ptr<B> object_; /**< 唯一拥有的具体后端或反应器对象，静态转发不额外擦除类型。 */
};

}  // namespace faio::io::detail
