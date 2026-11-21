/**
 * @file faio/coroutine/composition.hpp
 * @brief 任务组合、词法任务组、共享结果与串行执行域的公开入口。
 */
#pragma once

#include "faio/detail/coroutine/composition.hpp"              /* IWYU pragma: export */
#include "faio/detail/coroutine/composition/race_in.hpp"      /* IWYU pragma: export */
#include "faio/detail/coroutine/scheduler/serial_domain.hpp"  /* IWYU pragma: export */
#include "faio/detail/coroutine/composition/shared_task.hpp"  /* IWYU pragma: export */
#include "faio/detail/coroutine/composition/task_group.hpp"   /* IWYU pragma: export */
#include "faio/detail/coroutine/composition/join_limited.hpp" /* IWYU pragma: export */
#include "faio/detail/coroutine/composition/result.hpp"       /* IWYU pragma: export */
#include "faio/detail/coroutine/composition/settle.hpp"       /* IWYU pragma: export */
