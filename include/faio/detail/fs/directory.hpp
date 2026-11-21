/**
 * @file faio/detail/fs/directory.hpp
 * @brief 目录项与异步枚举。
 */
#pragma once
#if defined(_WIN32)
#include "faio/detail/fs/platform/windows/directory.hpp"
#else
#include "faio/detail/fs/platform/unix/directory.hpp"
#endif
