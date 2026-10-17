/**
 * @file faio/detail/sync.hpp
 * @brief faio 公共接口：同步协议与数据落盘。
 * @details 供 faio 各模块复用的头文件接口；具体所有权、错误与线程访问约束见类型及成员说明。
 */
#ifndef FAIO_DETAIL_SYNC_HPP
#define FAIO_DETAIL_SYNC_HPP
#include "faio/detail/sync/barrier.hpp"
#include "faio/detail/sync/condition_variable.hpp"
#include "faio/detail/sync/latch.hpp"
#include "faio/detail/sync/mpsc.hpp"
#include "faio/detail/sync/mutex.hpp"
#include "faio/detail/sync/semaphore.hpp"
#endif
