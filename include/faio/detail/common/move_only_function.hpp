/**
 * @file faio/detail/common/move_only_function.hpp
 * @brief 公共类型、错误与基础工具：仅移动可调用对象。
 * @details 供 faio 各模块复用的头文件接口；具体所有权、错误与线程访问约束见类型及成员说明。
 */
#pragma once
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace faio {
/** @brief 可移动、不可复制的拥有型回调，供尚无 std::move_only_function 的
 * libc++ 使用。
 * @details 小对象保存在64字节内联区；只有过大、过度对齐或可能抛出的移动构造
 * 才分配一次稳定对象。移动与析构不分配，任务接受后可以无异常地交接所有权。
 * 空回调不可调用，与 std::move_only_function 的前置条件相同。
 */
template <class Signature>
class move_only_function;

template <class R, class... Args>
class move_only_function<R(Args...)> {
 public:
  static constexpr std::size_t inline_capacity = 64;  ///< 可调用对象的内联存储容量，单位为字节。

 private:
  struct table {
    R (*invoke)(void*, Args&&...);  ///< 调用擦除类型后的目标对象，参数按原签名转发。

    void (*destroy)(void*) noexcept;  ///< 销毁擦除类型后的目标对象并释放其拥有的存储。

    void (*move)(void*, void*) noexcept;  ///< 将目标对象转移到另一存储位置，源对象随后不再拥有目标。
  };

  template <class F>
  static constexpr bool fits =
      sizeof(F) <= inline_capacity && alignof(F) <= alignof(std::max_align_t)
      && std::is_nothrow_move_constructible_v<F>;

  template <class F>
  static F& target(void* storage) noexcept {
    if constexpr (fits<F>)
      return *std::launder(reinterpret_cast<F*>(storage));
    else
      return **std::launder(reinterpret_cast<std::unique_ptr<F>*>(storage));
  }

  template <class F>
  inline static constexpr table operations{
      [](void* object, Args&&... args) -> R {
        if constexpr (std::is_void_v<R>)
          std::invoke(target<F>(object), std::forward<Args>(args)...);
        else
          return std::invoke(target<F>(object), std::forward<Args>(args)...);
      },
      [](void* object) noexcept {
        if constexpr (fits<F>)
          std::destroy_at(std::launder(reinterpret_cast<F*>(object)));
        else
          std::destroy_at(std::launder(reinterpret_cast<std::unique_ptr<F>*>(object)));
      },
      [](void* from, void* to) noexcept {
        if constexpr (fits<F>) {
          auto* source = std::launder(reinterpret_cast<F*>(from));
          std::construct_at(reinterpret_cast<F*>(to), std::move(*source));
          std::destroy_at(source);
        } else {
          auto* source = std::launder(reinterpret_cast<std::unique_ptr<F>*>(from));
          std::construct_at(reinterpret_cast<std::unique_ptr<F>*>(to), std::move(*source));
          std::destroy_at(source);
        }
      }};

 public:
  template <class F>
  static constexpr bool stores_inline = fits<std::decay_t<F>>;

  move_only_function() noexcept = default;

  move_only_function(std::nullptr_t) noexcept {}

  template <class F>
    requires(!std::same_as<std::remove_cvref_t<F>, move_only_function>)
            && std::is_invocable_r_v<R, std::decay_t<F>&, Args...>
  move_only_function(F&& function) {
    using callable = std::decay_t<F>;
    if constexpr (fits<callable>)
      std::construct_at(reinterpret_cast<callable*>(storage_), std::forward<F>(function));
    else
      std::construct_at(reinterpret_cast<std::unique_ptr<callable>*>(storage_),
                        std::make_unique<callable>(std::forward<F>(function)));
    table_ = &operations<callable>;
  }

  move_only_function(const move_only_function&) = delete;

  move_only_function& operator=(const move_only_function&) = delete;

  move_only_function(move_only_function&& other) noexcept
      : table_(std::exchange(other.table_, nullptr)) {
    if (table_)
      table_->move(other.storage_, storage_);
  }

  move_only_function& operator=(move_only_function&& other) noexcept {
    if (this != &other) {
      reset();
      table_ = std::exchange(other.table_, nullptr);
      if (table_)
        table_->move(other.storage_, storage_);
    }
    return *this;
  }

  ~move_only_function() { reset(); }

  explicit operator bool() const noexcept { return table_ != nullptr; }

  R operator()(Args... args) { return table_->invoke(storage_, std::forward<Args>(args)...); }

  /// @brief 按该类型约定释放或清空当前状态，重复调用遵循对象的空状态规则。
  void reset() noexcept {
    if (table_) {
      table_->destroy(storage_);
      table_ = nullptr;
    }
  }

 private:
  alignas(std::max_align_t) std::byte storage_[inline_capacity];  ///< 按最大基本对齐存放小型可调用对象的内联字节区。
  const table* table_{};  ///< 当前可调用类型的调用、析构和移动函数表；空指针表示未绑定。
};
}
