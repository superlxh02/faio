/**
 * @file faio/detail/fs/file.hpp
 * @brief 异步文件对象。
 */
#pragma once
#if defined(_WIN32)
#include "faio/detail/fs/platform/windows/file.hpp"
#else
#include "faio/detail/fs/platform/unix/file.hpp"
#endif
