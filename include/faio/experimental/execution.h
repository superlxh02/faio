/**
 * @file faio/experimental/execution.h
 * @brief 将同步调用交给隔离线程执行的协程桥接：发送器接收器入口。
 * @details 同步工作由独立阻塞服务执行；有界队列以错误返回背压。停止仅记录取消原因，已开始的借用型调用须真实返回后才发布完成并恢复协程。
 */
#ifndef FAIO_EXPERIMENTAL_EXECUTION_H
#define FAIO_EXPERIMENTAL_EXECUTION_H
#include "faio/detail/experimental/execution/run.hpp"
#include "faio/detail/experimental/execution/task_sender.hpp"
#include "faio/detail/experimental/execution/spawn_sender.hpp"
#include "faio/detail/experimental/execution/default_runtime.hpp"
#endif
