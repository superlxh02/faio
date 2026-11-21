/**
 * @file faio/detail/net/pipe.hpp
 * @brief 管道与命名管道。
 */
#pragma once
#if !defined(_WIN32)
#include "faio/detail/net/platform/unix/pipe.hpp"
#endif
