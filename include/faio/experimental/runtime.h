/**
 * @file faio/experimental/runtime.h
 * @brief 运行时调度、线程协调与定时驱动：运行时驱动器。
 * @details 运行时按所属线程区分私有状态与跨线程发布状态。任务接纳先登记完成责任，关闭先停止接纳再排空；休眠登记后复查工作以避免丢失唤醒。
 */
#ifndef FAIO_EXPERIMENTAL_RUNTIME_H
#define FAIO_EXPERIMENTAL_RUNTIME_H
#include "faio/detail/experimental/runtime_facade.hpp"
#endif
