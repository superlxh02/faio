/**
 * @file faio/detail/io/io.hpp
 * @brief IO 模块入口。
 */
#pragma once
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
