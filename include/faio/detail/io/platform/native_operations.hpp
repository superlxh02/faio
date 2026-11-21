/**
 * @file faio/detail/io/platform/native_operations.hpp
 * @brief 原生操作适配。
 */
#pragma once
#if defined(_WIN32)
#include "faio/detail/io/platform/windows/native_operations.hpp"
#else
#include "faio/detail/io/platform/unix/native_operations.hpp"
#endif
