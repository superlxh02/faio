/**
 * @file faio/detail/common/move_only_function.hpp
 * @brief 只可移动的可调用对象。
 */
#pragma once
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace faio {
/**
 *  @brief 持有可调用对象的回调，只能移动，不能复制；供尚无 std::move_only_function 的
 * libc++ 使用。
 * @details 小对象保存在 64 字节内联区；对象过大、对齐要求过高或移动构造可能抛出异常时，
 * 才在堆上分配地址固定的对象。移动与析构不分配内存，任务被接纳后可无异常地转移所有权。
 * 空回调不可调用，与 std::move_only_function 的前置条件相同。
 */
template <class Signature>
class move_only_function;

template <class R, class... Args>
class move_only_function<R(Args...)> {
 public:
  static constexpr std::size_t inline_capacity = 64; /**< 可调用对象的内联存储容量，单位为字节。 */

 private:
  /** @brief 保存调用、销毁和移动函数的操作表。 */
  struct table {
    R (*invoke)(void*, Args&&...); /**< 通过类型擦除调用目标对象，并按原签名转发参数。 */

    void (*destroy)(void*) noexcept; /**< 通过类型擦除销毁目标对象，并释放其持有的存储。 */

    void (*move)(void*, void*) noexcept; /**< 将目标对象移入另一存储位置，源位置不再持有目标。 */
  };

  template <class F>
  static constexpr bool fits =
      sizeof(F) <= inline_capacity && alignof(F) <= alignof(std::max_align_t)
      && std::is_nothrow_move_constructible_v<F>;

  /** @brief 取得存储中的目标可调用对象。 */
  template <class F>
  static F& target(void* storage) noexcept {
    if constexpr (fits<F>)
      return *std::launder(reinterpret_cast<F*>(storage));
    else
      return **std::launder(reinterpret_cast<std::unique_ptr<F>*>(storage));
  }

  template <class F>
  static R invoke_target(void* object, Args&&... args) {
    if constexpr (std::is_void_v<R>)
      std::invoke(target<F>(object), std::forward<Args>(args)...);
    else
      return std::invoke(target<F>(object), std::forward<Args>(args)...);
  }

  template <class F>
  static void destroy_target(void* object) noexcept {
    if constexpr (fits<F>)
      std::destroy_at(std::launder(reinterpret_cast<F*>(object)));
    else
      std::destroy_at(std::launder(reinterpret_cast<std::unique_ptr<F>*>(object)));
  }

  template <class F>
  static void move_target(void* from, void* to) noexcept {
    if constexpr (fits<F>) {
      auto* source = std::launder(reinterpret_cast<F*>(from));
      std::construct_at(reinterpret_cast<F*>(to), std::move(*source));
      std::destroy_at(source);
    } else {
      auto* source = std::launder(reinterpret_cast<std::unique_ptr<F>*>(from));
      std::construct_at(reinterpret_cast<std::unique_ptr<F>*>(to), std::move(*source));
      std::destroy_at(source);
    }
  }

  // Named targets avoid serializing lambda closures in GCC's module cache.
  template <class F>
  inline static constexpr table operations{&invoke_target<F>, &destroy_target<F>, &move_target<F>};

 public:
  template <class F>
  static constexpr bool stores_inline = fits<std::decay_t<F>>;

  /** @brief 构造空回调。 */
  move_only_function() noexcept = default;

  /** @brief 用 nullptr 构造空回调。 */
  move_only_function(std::nullptr_t) noexcept {}

  /** @brief 构造回调并持有传入的可调用对象。 */
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

  /** @brief 禁止复制可调用对象的所有权。 */
  move_only_function(const move_only_function&) = delete;

  /** @brief 禁止复制赋值。 */
  move_only_function& operator=(const move_only_function&) = delete;

  /** @brief 接管可调用对象，并将源对象置空。 */
  move_only_function(move_only_function&& other) noexcept
      : table_(std::exchange(other.table_, nullptr)) {
    if (table_)
      table_->move(other.storage_, storage_);
  }

  /** @brief 释放原有可调用对象，再接管源对象。 */
  move_only_function& operator=(move_only_function&& other) noexcept {
    if (this != &other) {
      reset();
      table_ = std::exchange(other.table_, nullptr);
      if (table_)
        table_->move(other.storage_, storage_);
    }
    return *this;
  }

  /** @brief 销毁持有的可调用对象。 */
  ~move_only_function() { reset(); }

  /** @brief 检查是否持有可调用对象。 */
  explicit operator bool() const noexcept { return table_ != nullptr; }

  R operator()(Args... args) { return table_->invoke(storage_, std::forward<Args>(args)...); }

  /** @brief 销毁持有的可调用对象并清空状态；可重复调用。 */
  void reset() noexcept {
    if (table_) {
      table_->destroy(storage_);
      table_ = nullptr;
    }
  }

 private:
  alignas(std::max_align_t)
      std::byte storage_[inline_capacity]; /**< 按最大基本对齐要求存放小型可调用对象的内联区。 */
  const table* table_{}; /**< 当前类型的调用、析构和移动函数表；空指针表示未绑定对象。 */
};
}  // namespace faio
