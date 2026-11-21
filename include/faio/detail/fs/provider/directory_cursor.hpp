/**
 * @file faio/detail/fs/provider/directory_cursor.hpp
 * @brief 原生目录游标。
 */
#pragma once
#if defined(_WIN32)
#include "faio/detail/fs/platform/windows/directory_cursor.hpp"
#elif defined(__linux__)
#include "faio/detail/fs/platform/linux/directory_cursor.hpp"
#endif
