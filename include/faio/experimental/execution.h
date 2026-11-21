/**
 * @file faio/experimental/execution.h
 * @brief 在 faio 协程和 Sender/Receiver 任务图之间转换与交付结果。
 * @details 在 faio 协程内调用 as_sender 和 spawn_sender 时，库会使用当前执行上下文。
 */
#ifndef FAIO_EXPERIMENTAL_EXECUTION_H
#define FAIO_EXPERIMENTAL_EXECUTION_H
#include "faio/detail/experimental/execution/run.hpp"
#include "faio/detail/experimental/execution/task_sender.hpp"
#include "faio/detail/experimental/execution/spawn_sender.hpp"
#include "faio/detail/experimental/execution/default_runtime.hpp"
#endif
