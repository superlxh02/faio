/**
 * @file faio/detail/net/platform/select.hpp
 * @brief 网络地址、socket 与传输协议：平台网络类型选择。
 * @details 网络封装保持 socket 资源租约与所属 IO 域。流、报文和半关闭语义分别处理，借用缓冲区在完成前保持有效，原生句柄宽度由平台类型保留。
 */
#pragma once
/// @brief 平台选择只在此收口；TCP/UDP 和公共算法不区分操作系统。
#if defined(_WIN32)
#include "faio/detail/net/platform/windows/socket.hpp"
#elif defined(__linux__)
#include "faio/detail/net/platform/linux/socket.hpp"
#elif defined(__APPLE__) || defined(__FreeBSD__)
#include "faio/detail/net/platform/macos/socket.hpp"
#else
#error "faio networking requires Linux, macOS/BSD or Windows"
#endif
