/**
 * @file faio/detail/io/platform/native_types.hpp
 * @brief 原生类型与句柄宽度。
 */
#pragma once
#if defined(_WIN32)
#include "faio/detail/io/platform/windows/native_types.hpp"
#else
#include "faio/detail/io/platform/unix/native_types.hpp"
#endif
