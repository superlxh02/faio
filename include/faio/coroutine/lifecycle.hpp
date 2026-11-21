/**
 * @file faio/coroutine/lifecycle.hpp
 * @brief 协程的异步清理、停止边界与触发式截止。
 */
#pragma once

#include "faio/detail/coroutine/composition/with_cleanup.hpp"    /* IWYU pragma: export */
#include "faio/detail/coroutine/composition/complete_before.hpp" /* IWYU pragma: export */
#include "faio/detail/coroutine/composition/with_stop_token.hpp" /* IWYU pragma: export */
