/**
 * @file faio/detail/fs/provider/vectored.hpp
 * @brief 分散聚集文件 I/O。
 */
#pragma once
#if defined(_WIN32)
#include "faio/detail/fs/platform/windows/vectored.hpp"
#else
#include "faio/detail/fs/platform/unix/vectored.hpp"
#endif
