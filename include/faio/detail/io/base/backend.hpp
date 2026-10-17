/**
 * @file faio/detail/io/base/backend.hpp
 * @brief 跨平台异步 IO：后端封装与提交协议。
 * @details 基础包装保留具体后端的静态类型与所有权，转发注册、提交、轮询及控制接口；不以回执数量推断业务完成或负载释放。
 */
#pragma once
#include "faio/detail/io/traits/backend.hpp"
#include <memory>
#include <utility>

namespace faio::io::detail {
/** @brief 后端合同的 CRTP 接口；显式对象参数保留派生拥有者的静态类型。
 * @details 不擦除对象类型，不保存函数表。可选能力在编译期选择，调用具体后端。
 */
template <class Derived>
class backend_api {
 protected:
  backend_api() = default;
  ~backend_api() = default;

 public:
  /// @brief 向具体后端登记长期资源身份；失败时不接管调用方的原生句柄。
  int attach(this Derived& self, native_registration handle, std::uint64_t key) noexcept {
    auto& object = self.object();
    if constexpr (requires { object.attach(handle, key); })
      return object.attach(handle, key);
    else {
      /// @brief 保留完整 Windows handle；仅 POSIX adapter 允许缩窄。
      if (handle.kind != native_handle_kind::posix_descriptor || handle.value > 0x7fffffffU)
        return -22;
      return object.attach(static_cast<int>(handle.value), key);
    }
  }

  /// @brief 向具体后端登记长期资源身份；失败时不接管调用方的原生句柄。
  int attach(this Derived& self, int handle, std::uint64_t key) noexcept {
    return self.attach({static_cast<std::uintptr_t>(handle), native_handle_kind::posix_descriptor},
                       key);
  }

  /// @brief 撤销原生资源登记，迟到事件仍须由上层按资源身份过滤。
  void detach(this Derived& self, native_registration handle) noexcept {
    auto& object = self.object();
    if constexpr (requires { object.detach(handle); })
      object.detach(handle);
    else if (handle.kind == native_handle_kind::posix_descriptor && handle.value <= 0x7fffffffU)
      object.detach(static_cast<int>(handle.value));
  }

  /// @brief 撤销原生资源登记，迟到事件仍须由上层按资源身份过滤。
  void detach(this Derived& self, int handle) noexcept {
    self.detach({static_cast<std::uintptr_t>(handle), native_handle_kind::posix_descriptor});
  }

  /// @brief 尝试接受稳定请求，明确区分已接受、暂时满载与拒绝。
  backend_submit_result try_submit(this Derived& self, backend_operation op) noexcept {
    return self.object().try_submit(op);
  }

  /// @brief 提交缓冲的写入或已准备的后端请求，失败保留尚未完成的责任。
  backend_flush_result flush(this Derived& self) noexcept { return self.object().flush(); }

  /// @brief 查询本后端的 poll 是否同时负责冲刷已准备的提交。
  bool poll_flushes_submissions(this const auto&) noexcept {
    using B = typename Derived::backend_type;
    if constexpr (requires { B::poll_flushes_submissions; })
      return static_cast<bool>(B::poll_flushes_submissions);
    else
      return false;
  }

  /// @brief 在给定输出容量与等待预算内消费一批事件，保留后续未消费事件。
  int poll(this Derived& self, std::span<backend_event> out, std::optional<int> timeout) noexcept {
    return self.object().poll(out, timeout);
  }

  /// @brief ACK 不代替原业务完成，后端始终收到完整代际及稳定 request。
  void request_cancel(this Derived& self, backend_operation op) noexcept {
    if constexpr (requires { self.object().request_cancel(op); })
      self.object().request_cancel(op);
    else
      self.object().request_cancel(op.token);
  }

  /// @brief 发送独立控制通知，打断底层等待以处理已经发布的工作。
  void wake(this const auto& self) noexcept { self.object().wake(); }

  /// @brief 停止接受新业务，按停止策略取消或保留已接受请求并启动排空。
  void begin_shutdown(this Derived& self) noexcept { self.object().begin_shutdown(); }

  /// @brief 未知 native 故障不能证明内核借用已解除，仍须 fail-fast。
  bool begin_failure(this Derived& self, int error) noexcept {
    if constexpr (requires { self.object().begin_failure(error); })
      return self.object().begin_failure(error);
    else {
      self.object().begin_shutdown();
      return !self.native();
    }
  }

  /// @brief 检查当前组件是否已经没有尚未归还的执行、内核引用与完成责任。
  bool quiescent(this const auto& self) noexcept { return self.object().quiescent(); }

  /// @brief 查询具体后端是否直接提供原生异步提交与完成。
  bool native(this const auto&) noexcept { return Derived::backend_type::native_proactor; }

  /// @brief 查询初始化时固定的原生能力，避免普通请求热路径重复探测平台。
  bool supports(this const auto& self, std::uint32_t kind) noexcept {
    return self.object().supports(kind);
  }

  /// @brief 返回实际后端的静态诊断名称。
  const char* name(this const auto& self) noexcept { return self.object().name(); }

  /// @brief 读取后端累计提交、完成与控制事件统计。
  backend_statistics statistics(this const auto& self) noexcept {
    return self.object().statistics();
  }
};

template <class B>
/// @brief 唯一拥有具体后端的稳定包装；公开接口经 CRTP 保留静态分派。
class basic_backend_box : public backend_api<basic_backend_box<B>> {
 public:
  using backend_type = B;  ///< 编译期选定的具体后端类型，转发时保留静态分派。

  explicit basic_backend_box(std::unique_ptr<B> object) noexcept
    requires backend_implementation<B>
      : object_(std::move(object)) {}

  basic_backend_box(basic_backend_box&&) noexcept = default;
  basic_backend_box& operator=(basic_backend_box&&) = delete;
  basic_backend_box(const basic_backend_box&) = delete;
  basic_backend_box& operator=(const basic_backend_box&) = delete;
  ~basic_backend_box() = default;

 private:
  friend class backend_api<basic_backend_box<B>>;

  /// @brief const owning handle 仍允许跨线程 wake；const 不改变所拥有后端的通知状态合同。
  B& object() const noexcept { return *object_; }

  std::unique_ptr<B> object_;  ///< 唯一拥有的具体后端或反应器对象，静态转发不额外擦除类型。
};

}
