/**
 * @file faio/detail/io/io.hpp
 * @brief 跨平台异步 IO：IO 模块入口。
 * @details 统一 IO 通过上下文、资源租约与等待器连接运行时；组合操作保留部分进度并在安全完成后归还负载及方向执行权。
 */
#pragma once
/** @brief IO 操作、缓冲区、流组合与原生资源适配的统一入口。 */
#include "faio/detail/io/awaiter/operations.hpp"
#include "faio/detail/io/awaiter/native_file.hpp"
#include "faio/detail/io/core/file_descriptor.hpp"
#include "faio/detail/io/traits.hpp"
#include "faio/detail/io/stream/adapters.hpp"
#include "faio/detail/io/stream/buffered.hpp"
#include "faio/detail/io/stream/endian.hpp"
#include "faio/detail/io/stream/memory_stream.hpp"
#if !defined(_WIN32)
#include "faio/detail/io/platform/unix/async_fd.hpp"
#endif
