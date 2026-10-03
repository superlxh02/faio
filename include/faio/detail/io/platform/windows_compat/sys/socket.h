/**
 * @file sys/socket.h
 * @brief Windows 编译时为既有公共 concept 提供 I/O 平台层的套接字类型。
 * @details 此兼容头仅通过 Windows 的目标 include 路径启用，POSIX 构建仍包含系统头。
 */
#pragma once
#include "faio/detail/io/platform/posix_types.hpp"
