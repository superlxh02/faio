/**
 * @file faio/detail/coroutine/composition.hpp
 * @brief 任务组合入口：结果句柄、结构化作用域及并发等待。
 */
#ifndef FAIO_DETAIL_COROUTINE_COMPOSITION_HPP
#define FAIO_DETAIL_COROUTINE_COMPOSITION_HPP

#include "faio/detail/coroutine/task.hpp"                    /* IWYU pragma: export */
#include "faio/detail/coroutine/composition/join_handle.hpp" /* IWYU pragma: export */
#include "faio/detail/coroutine/launch/observed_task.hpp"    /* IWYU pragma: export */
#include "faio/detail/coroutine/composition/scope.hpp"       /* IWYU pragma: export */
#include "faio/detail/coroutine/composition/when_all.hpp"    /* IWYU pragma: export */
#include "faio/detail/coroutine/composition/select.hpp"      /* IWYU pragma: export */

#include "faio/detail/coroutine/composition/settled.hpp"        /* IWYU pragma: export */
#include "faio/detail/coroutine/composition/select_success.hpp" /* IWYU pragma: export */

#endif
