#ifndef FAIO_DETAIL_COROUTINE_HPP
#define FAIO_DETAIL_COROUTINE_HPP

// 用户协程接口；内部上下文、等待设施与根任务由实现头文件自行引入。
// 默认运行时入口由 faio/faio.hpp 提供。
#include "faio/detail/coroutine/combinators.hpp"
#include "faio/detail/coroutine/join_handle.hpp"
#include "faio/detail/coroutine/scope.hpp"
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/coroutine/this_coro.hpp"

#endif  // FAIO_DETAIL_COROUTINE_HPP
