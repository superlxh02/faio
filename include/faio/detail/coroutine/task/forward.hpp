/**
 * @file faio/detail/coroutine/task/forward.hpp
 * @brief 任务类型的轻量前置声明；默认调度器仍由静态引用模块定义。
 */
#ifndef FAIO_DETAIL_COROUTINE_TASK_FORWARD_HPP
#define FAIO_DETAIL_COROUTINE_TASK_FORWARD_HPP

#include "faio/detail/coroutine/scheduler/reference.hpp"

namespace faio {
/** @brief 用户任务的默认参数只在此前置声明给出，避免实现头重复声明。 */
template <class T = void, class S = default_scheduler>
class task;
}  // namespace faio
#endif
