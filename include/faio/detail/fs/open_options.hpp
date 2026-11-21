/**
 * @file faio/detail/fs/open_options.hpp
 * @brief 文件打开选项。
 */
#pragma once
#if defined(_WIN32)
#include "faio/detail/fs/platform/windows/open_options.hpp"
#else
#include "faio/detail/fs/platform/unix/open_options.hpp"
#endif
