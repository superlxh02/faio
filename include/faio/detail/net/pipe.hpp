/**
 * @file faio/detail/net/pipe.hpp
 * @brief 网络地址、socket 与传输协议：管道与命名管道。
 * @details 网络封装保持 socket 资源租约与所属 IO 域。流、报文和半关闭语义分别处理，借用缓冲区在完成前保持有效，原生句柄宽度由平台类型保留。
 */
#pragma once
/// @brief pipe/FIFO 与 AF_UNIX socket 分开组织，公共命名空间保留源码兼容。
#if !defined(_WIN32)
#include "faio/detail/net/platform/unix/pipe.hpp"
#endif
