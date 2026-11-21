/**
 * @file faio/detail/coroutine/lifetime/tracker_fwd.hpp
 * @brief 任务组借用身份与当前线程指针，不引入完成服务或宿主。
 */
#pragma once

namespace faio::detail {
/** @brief 结构化子任务计数器。 */
struct task_tracker;
}  // namespace faio::detail
