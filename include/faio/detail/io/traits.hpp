/**
 * @file faio/detail/io/traits.hpp
 * @brief 跨平台异步 IO：IO 类型约束入口。
 * @details 统一 IO 通过上下文、资源租约与等待器连接运行时；组合操作保留部分进度并在安全完成后归还负载及方向执行权。
 */
#pragma once
/** @brief 对外的异步流接口约束；后端和 reactor 约束仅供内部装配。 */
#include "faio/detail/io/traits/async_read.hpp"
#include "faio/detail/io/traits/async_write.hpp"
#include "faio/detail/io/traits/async_seek.hpp"
#include "faio/detail/io/traits/async_buf_read.hpp"
