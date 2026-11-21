/**
 * @file faio/detail/net/platform/select.hpp
 * @brief 平台网络类型选择。
 */
#pragma once
#if defined(_WIN32)
#include "faio/detail/net/platform/windows/socket.hpp"
#elif defined(__linux__)
#include "faio/detail/net/platform/linux/socket.hpp"
#elif defined(__APPLE__) || defined(__FreeBSD__)
#include "faio/detail/net/platform/unix/bsd_socket.hpp"
#else
#error "faio networking requires Linux, macOS/BSD or Windows"
#endif
