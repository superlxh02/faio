/**
 * @file faio/io/native.hpp
 * @brief 在 Unix 平台管理原生描述符和等待描述符就绪的公开入口。
 */
#pragma once

#if !defined(_WIN32)
#include "faio/detail/io/platform/unix/async_fd.hpp"  // IWYU pragma: export
#include "faio/detail/io/platform/unix/owned_fd.hpp"  // IWYU pragma: export
#endif
