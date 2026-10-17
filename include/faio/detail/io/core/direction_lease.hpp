/**
 * @file faio/detail/io/core/direction_lease.hpp
 * @brief 跨平台异步 IO：组合 IO 方向租约。
 * @details 核心层维护长期资源身份、稳定操作代际和生命周期。资源首次绑定后保持所属域，关闭、取消、完成与槽位复用必须遵循同一排空协议。
 */
#pragma once
#include "faio/detail/io/core/domain.hpp"
#include "faio/detail/common/error.hpp"
#include <system_error>
#include <utility>

namespace faio::io::detail {
/** @brief 组合 read_exact/write_all 的方向租约；持有资源，销毁自动释放 gate。
 */
class direction_lease {
 public:
  direction_lease(resource_ptr resource, Interest interest, void* owner) noexcept
      : resource_(std::move(resource)), interest_(interest), owner_(owner) {}

  direction_lease(const direction_lease&) = delete;

  direction_lease& operator=(const direction_lease&) = delete;

  direction_lease(direction_lease&& other) noexcept
      : resource_(std::move(other.resource_)), interest_(other.interest_), owner_(other.owner_) {}

  ~direction_lease() {
    if (resource_ && resource_->owner)
      resource_->owner->unreserve(*resource_, interest_, owner_);
  }

 private:
  resource_ptr resource_;  ///< 共享资源控制块租约，跨挂起固定句柄身份及 IO 域归属。
  Interest interest_;  ///< 当前等待或预留关注的读写方向集合。
  void* owner_;  ///< 组合操作的稳定身份，用于验证读写方向的排他预留。
};

/**
 * @brief 组合协程内部的借用方向释放守卫，不重复持有资源引用计数。
 * @param resource 由同一协程帧的按值 resource_ptr 参数拥有的稳定控制块。
 * @param interest 本组合操作持有的方向；析构仍进入原 domain 的线性化锁。
 * @param owner 位于同一组合帧中的稳定方向身份。
 * @details 仅适用于资源参数在整个守卫生命周期内不被移动/清空的内部组合实现。
 *          局部守卫在退出函数体时先销毁，协程参数在 promise 销毁后才销毁；
 *          所以取消、异常和正常返回都先释放方向，再归还最后资源租约。
 *          对外取得方向租约仍返回拥有型 direction_lease，保持原所有权语义。
 */
class borrowed_direction_lease {
 public:
  borrowed_direction_lease(resource_state& resource, Interest interest, void* owner) noexcept
      : resource_(resource), interest_(interest), owner_(owner) {}

  borrowed_direction_lease(const borrowed_direction_lease&) = delete;

  borrowed_direction_lease& operator=(const borrowed_direction_lease&) = delete;

  ~borrowed_direction_lease() {
    /// @brief owner 在首次 bind 发布后不再变化；只取得一次稳定的 domain 指针。
    /// @brief 复用半关闭和身份匹配协议。
    if (auto* domain = resource_.owner.get())
      domain->unreserve(resource_, interest_,
                        owner_);
  }

 private:
  resource_state& resource_;  ///< 只借用地址；创建处的协程参数保持强拥有租约。
  Interest interest_;  ///< 不更改方向排他范围和释放的线性化点。
  void* owner_;  ///< 无论首操作是否取得方向，都只释放匹配的身份。
};

/// @brief 为组合 IO 取得指定方向的 RAII 预留租约。
inline expected<direction_lease> reserve_direction(resource_ptr resource,
                                                   Interest interest,
                                                   void* owner) noexcept {
  if (!resource)
    return std::unexpected{make_error(EBADF)};
  if (!resource->owner) {
    /// @brief 与普通 await 一致，runtime 外导入的资源由首次执行时的 context 固定归属。
    auto context = io_context::current();
    if (!context)
      return std::unexpected{make_error(ECANCELED)};
    try {
      context.domain()->bind(resource);
    } catch (const std::system_error& error) {
      if (error.code().value() != EXDEV || !resource->owner)
        return std::unexpected{make_error(error.code().value())};
      /// @brief 并发首次绑定已由其他 worker 获胜，后续 reserve 使用它发布的不可变 owner。
    } catch (...) {
      return std::unexpected{make_error(ENOMEM)};
    }
  }
  if (auto result = resource->owner->reserve(*resource, interest, owner); !result)
    return std::unexpected{result.error()};
  return direction_lease{std::move(resource), interest, owner};
}

}
