/**
 * @file faio/detail/io/core/domain.hpp
 * @brief 跨平台异步 IO：资源与操作状态域。
 * @details 核心层维护长期资源身份、稳定操作代际和生命周期。资源首次绑定后保持所属域，关闭、取消、完成与槽位复用必须遵循同一排空协议。
 */
#pragma once
/** @brief 稳定 domain 接口的装配入口；共享状态与上下文不依赖具体操作系统。 */
#if defined(_WIN32)
#include "faio/detail/io/platform/windows/domain.hpp"
#else
#include "faio/detail/io/platform/unix/domain.hpp"
#endif
#include "faio/detail/io/core/resource_impl.hpp"
#include "faio/detail/io/core/context_impl.hpp"
#include "faio/detail/io/core/direction_lease.hpp"
