/**
 * @file faio/detail/fs/metadata.hpp
 * @brief 文件元数据。
 */
#pragma once
#if defined(_WIN32)
#include "faio/detail/fs/platform/windows/metadata.hpp"
#else
#include "faio/detail/fs/platform/unix/metadata.hpp"
#endif
