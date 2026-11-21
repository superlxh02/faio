/**
 * @file faio/detail/io/awaiter/operations.hpp
 * @brief 操作工厂与组合接口。
 */
#pragma once
#include "faio/detail/io/awaiter/accept.hpp"
#include "faio/detail/io/awaiter/cancel.hpp"
#include "faio/detail/io/awaiter/close.hpp"
#include "faio/detail/io/awaiter/cmd_sock.hpp"
#include "faio/detail/io/awaiter/connect.hpp"
#include "faio/detail/io/awaiter/fsync.hpp"
#include "faio/detail/io/awaiter/open.hpp"
#include "faio/detail/io/awaiter/read.hpp"
#include "faio/detail/io/awaiter/readv.hpp"
#include "faio/detail/io/awaiter/recv.hpp"
#include "faio/detail/io/awaiter/recvfrom.hpp"
#include "faio/detail/io/awaiter/recvmsg.hpp"
#include "faio/detail/io/awaiter/send.hpp"
#include "faio/detail/io/awaiter/sendmsg.hpp"
#include "faio/detail/io/awaiter/sendto.hpp"
#include "faio/detail/io/awaiter/shutdown.hpp"
#include "faio/detail/io/awaiter/socket.hpp"
#include "faio/detail/io/awaiter/write.hpp"
#include "faio/detail/io/awaiter/writev.hpp"
#include "faio/detail/io/engine.hpp"

#include "faio/detail/io/awaiter/ready.hpp"

#if defined(_WIN32)
#include "faio/detail/io/platform/windows/operation_overloads.hpp"
#endif
#if defined(__linux__)
#include "faio/detail/io/platform/linux/operation_overloads.hpp"
#endif
