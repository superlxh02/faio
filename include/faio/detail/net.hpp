/**
 * @file faio/detail/net.hpp
 * @brief faio 公共接口：网络模块入口。
 * @details 供 faio 各模块复用的头文件接口；具体所有权、错误与线程访问约束见类型及成员说明。
 */
#pragma once
#include "faio/detail/net/accept_options.hpp"
#include "faio/detail/net/address.hpp"
#include "faio/detail/net/resolver.hpp"
#include "faio/detail/net/socket_options.hpp"
#include "faio/detail/net/tcp.hpp"
#include "faio/detail/net/udp.hpp"
#include "faio/detail/net/unix_domain.hpp"
#include "faio/detail/net/pipe.hpp"
