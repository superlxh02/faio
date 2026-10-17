/**
 * @file faio/detail/io/base/reactor.hpp
 * @brief 跨平台异步 IO：就绪反应器。
 * @details 基础包装保留具体后端的静态类型与所有权，转发注册、提交、轮询及控制接口；不以回执数量推断业务完成或负载释放。
 */
#pragma once
#include "faio/detail/io/traits/reactor.hpp"
#include <memory>
#include <utility>

namespace faio::io::detail {
#if defined(FAIO_REACTOR_CONTROL_TESTS) && FAIO_REACTOR_CONTROL_TESTS
/// @brief 仅单翻译单元的 reactor 契约测试启用这些确定性竞态测试入口。
enum class reactor_control_test_point { poll_after_consume, wake_before_syscall };
#endif

/// @brief CRTP readiness 合同，显式对象参数直接调用具体 reactor。
template <class Derived>
class reactor_api {
 protected:
  reactor_api() = default;
  ~reactor_api() = default;

 public:
  /// @brief 向具体后端登记长期资源身份；失败时不接管调用方的原生句柄。
  int attach(this Derived& self, int fd, std::uint64_t key) noexcept {
    return self.object().attach(fd, key);
  }

  /// @brief 撤销原生资源登记，迟到事件仍须由上层按资源身份过滤。
  void detach(this Derived& self, int fd) noexcept { self.object().detach(fd); }

  /// @brief 在给定输出容量与等待预算内消费一批事件，保留后续未消费事件。
  int poll(this Derived& self,
           std::span<readiness_event> out,
           std::optional<int> timeout) noexcept {
    return self.object().poll(out, timeout);
  }

  /// @brief 在具体 reactor 内直接生成调用方事件，映射器在编译期内联。
  template <class Event, class Map>
  int poll_into(this Derived& self,
                std::span<Event> out,
                std::optional<int> timeout,
                Map map) noexcept
    requires requires { self.object().poll_into(out, timeout, map); }
  {
    return self.object().poll_into(out, timeout, map);
  }

  /// @brief 发送独立控制通知，打断底层等待以处理已经发布的工作。
  void wake(this const auto& self) noexcept { self.object().wake(); }

  /// @brief 返回实际后端的静态诊断名称。
  const char* name(this const auto&) noexcept { return Derived::reactor_type::backend_name; }
};

template <class R>
/// @brief 唯一拥有具体就绪反应器，移动只转交所有权而不迁移原生队列对象。
class basic_reactor_box : public reactor_api<basic_reactor_box<R>> {
 public:
  using reactor_type = R;  ///< 编译期选定的具体就绪反应器类型。

  explicit basic_reactor_box(std::unique_ptr<R> object) noexcept
    requires reactor_implementation<R>
      : object_(std::move(object)) {}

  basic_reactor_box(basic_reactor_box&&) noexcept = default;
  basic_reactor_box& operator=(basic_reactor_box&&) = delete;
  basic_reactor_box(const basic_reactor_box&) = delete;
  basic_reactor_box& operator=(const basic_reactor_box&) = delete;
  ~basic_reactor_box() = default;

 private:
  friend class reactor_api<basic_reactor_box<R>>;

  /// @brief 取得由当前包装唯一拥有的具体实现对象，保留静态类型转发。
  R& object() const noexcept { return *object_; }

  std::unique_ptr<R> object_;  ///< 唯一拥有的具体后端或反应器对象，静态转发不额外擦除类型。
};

}
