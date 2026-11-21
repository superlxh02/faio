/**
 * @file faio/detail/io/core/fwd.hpp
 * @brief 共享状态前置声明。
 */
#pragma once
#include <memory>

namespace faio::io::detail {
/** @brief I/O 资源与操作的归属域。 */
class io_domain;
/** @brief I/O 资源放置组。 */
class io_placement_group;
/** @brief 原生 I/O 资源的共享状态。 */
struct resource_state;
/** @brief I/O 操作的提交与完成状态。 */
struct operation_state;
using resource_ptr = std::shared_ptr<resource_state>;
}  // namespace faio::io::detail
