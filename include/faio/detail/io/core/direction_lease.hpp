/**
 * @file faio/detail/io/core/direction_lease.hpp
 * @brief 组合 IO 方向租约。
 */
#pragma once
#include "faio/detail/io/core/domain.hpp"
#include "faio/detail/common/error.hpp"
#include <system_error>
#include <utility>

namespace faio::io::detail {
/**  @brief 组合 read_exact/write_all 的方向租约；持有资源，销毁自动释放 gate。 */
class direction_lease {
 public:
  /** @brief 构造 I/O 方向独占租约。 */
  direction_lease(resource_ptr resource, Interest interest, void* owner) noexcept
      : resource_(std::move(resource)), interest_(interest), owner_(owner) {}

  /** @brief 禁止复制构造 I/O 方向独占租约。 */
  direction_lease(const direction_lease&) = delete;

  /** @brief 禁止复制赋值。 */
  direction_lease& operator=(const direction_lease&) = delete;

  /** @brief 移动构造 I/O 方向独占租约。 */
  direction_lease(direction_lease&& other) noexcept
      : resource_(std::move(other.resource_)), interest_(other.interest_), owner_(other.owner_) {}

  /** @brief 销毁 I/O 方向独占租约。 */
  ~direction_lease() {
    if (resource_ && resource_->owner)
      resource_->owner->unreserve(*resource_, interest_, owner_);
  }

 private:
  resource_ptr resource_; /**< 共享资源控制块的租约，在挂起期间保持句柄身份及所属 I/O 域不变。 */
  Interest interest_;     /**< 当前等待或预留关注的读写方向集合。 */
  void* owner_;           /**< 组合操作的稳定身份，用于验证读写方向的排他预留。 */
};

/**
 * @brief 组合协程内部借用资源的方向释放守卫，不再增加资源引用计数。
 * @param resource 由同一协程帧的按值 resource_ptr 参数拥有的稳定控制块。
 * @param interest 本组合操作持有的方向；析构仍进入原 domain 的线性化锁。
 * @param owner 位于同一组合帧中的稳定方向身份。
 * @details 仅适用于资源参数在整个守卫生命周期内不被移动/清空的内部组合实现。
 *          局部守卫在退出函数体时先销毁，协程参数在 promise 销毁后才销毁；
 *          所以取消、异常和正常返回都先释放方向，再归还最后资源租约。
 *          对外取得方向租约仍返回持有资源所有权的 direction_lease，保持原所有权语义。
 */
class borrowed_direction_lease {
 public:
  /** @brief 构造借用的 I/O 方向租约。 */
  borrowed_direction_lease(resource_state& resource, Interest interest, void* owner) noexcept
      : resource_(resource), interest_(interest), owner_(owner) {}

  /** @brief 禁止复制构造借用的 I/O 方向租约。 */
  borrowed_direction_lease(const borrowed_direction_lease&) = delete;

  /** @brief 禁止复制赋值。 */
  borrowed_direction_lease& operator=(const borrowed_direction_lease&) = delete;

  /** @brief 销毁借用的 I/O 方向租约。 */
  ~borrowed_direction_lease() {
    if (auto* domain = resource_.owner.get())
      domain->unreserve(resource_, interest_, owner_);
  }

 private:
  resource_state& resource_; /**< 只借用地址；创建处的协程参数持续持有强引用租约。 */
  Interest interest_;        /**< 不更改方向排他范围和释放的线性化点。 */
  void* owner_;              /**< 无论首操作是否取得方向，都只释放匹配的身份。 */
};

/** @brief 为组合 IO 取得指定方向的 RAII 预留租约。 */
inline expected<direction_lease> reserve_direction(resource_ptr resource,
                                                   Interest interest,
                                                   void* owner) noexcept {
  if (!resource)
    return std::unexpected{make_error(EBADF)};
  if (!resource->owner) {
    auto context = io_context::current();
    if (!context)
      return std::unexpected{make_error(ECANCELED)};
    try {
      context.domain()->bind(resource);
    } catch (const std::system_error& error) {
      if (error.code().value() != EXDEV || !resource->owner)
        return std::unexpected{make_error(error.code().value())};
    } catch (...) {
      return std::unexpected{make_error(ENOMEM)};
    }
  }
  if (auto result = resource->owner->reserve(*resource, interest, owner); !result)
    return std::unexpected{result.error()};
  return direction_lease{std::move(resource), interest, owner};
}

}  // namespace faio::io::detail
