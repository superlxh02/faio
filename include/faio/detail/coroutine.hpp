/**
 * @file faio/detail/coroutine.hpp
 * @brief faio 公共接口：协程模块入口。
 * @details 供 faio 各模块复用的头文件接口；具体所有权、错误与线程访问约束见类型及成员说明。
 */
#ifndef FAIO_DETAIL_COROUTINE_HPP
#define FAIO_DETAIL_COROUTINE_HPP

/// @brief 用户协程接口；内部上下文、等待设施与根任务由实现头文件自行引入。
/// 默认运行时入口由 faio/faio.hpp 提供。
#include "faio/detail/coroutine/combinators.hpp"
#include "faio/detail/coroutine/join_handle.hpp"
#include "faio/detail/coroutine/scope.hpp"
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/coroutine/this_coro.hpp"
#endif
