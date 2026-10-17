/**
 * @file faio/detail/io/core/fwd.hpp
 * @brief 跨平台异步 IO：共享状态前置声明。
 * @details 核心层维护长期资源身份、稳定操作代际和生命周期。资源首次绑定后保持所属域，关闭、取消、完成与槽位复用必须遵循同一排空协议。
 */
#pragma once
#include <memory>

namespace faio::io::detail {
class io_domain;
class io_placement_group;
struct resource_state;
struct operation_state;
using resource_ptr = std::shared_ptr<resource_state>;
}
