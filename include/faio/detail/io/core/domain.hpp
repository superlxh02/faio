/**
 * @file faio/detail/io/core/domain.hpp
 * @brief 资源与操作状态域。
 */
#pragma once
#if defined(_WIN32)
#include "faio/detail/io/platform/windows/domain.hpp"
#else
#include "faio/detail/io/platform/unix/domain.hpp"
#endif
#include "faio/detail/io/core/resource_impl.hpp"
#include "faio/detail/io/core/context_impl.hpp"
#include "faio/detail/io/core/direction_lease.hpp"

#include "faio/detail/runtime/common/coroutine_integration.hpp"
