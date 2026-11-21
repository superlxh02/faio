/**
 * @file faio/detail/fs/operations.hpp
 * @brief 操作工厂与组合接口。
 */
#pragma once
#if defined(_WIN32)
#include "faio/detail/fs/platform/windows/operations.hpp"
#else
#include "faio/detail/fs/platform/unix/operations.hpp"
#endif
