/**
 * @file faio/detail/net/platform/socket_association.hpp
 * @brief 网络地址、socket 与传输协议：socket 所属后端关联。
 * @details 网络封装保持 socket 资源租约与所属 IO 域。流、报文和半关闭语义分别处理，借用缓冲区在完成前保持有效，原生句柄宽度由平台类型保留。
 */
#pragma once
#if defined(_WIN32)
#include "faio/detail/net/platform/windows/socket_association.hpp"
#else
#include "faio/detail/net/platform/unix/socket_association.hpp"
#endif
