/**
 * @file faio/detail/runtime/common/coroutine_integration.hpp
 * @brief 完整默认调度器、外部宿主与 IO 唤醒的集中装配边界。
 */
#pragma once
#include "faio/detail/runtime/multi_thread/scheduler/domain_scheduler.hpp"
#include "faio/detail/coroutine/context/context_scope.hpp"
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
#include "faio/detail/runtime/execution_adapter/external_work.hpp"
#endif
#include "faio/detail/runtime/common/task_lifetime_service_impl.hpp"
#if defined(FAIO_EXPERIMENTAL_HAS_EXECUTION) && FAIO_EXPERIMENTAL_HAS_EXECUTION
#include "faio/detail/runtime/execution_adapter/task_completion_service_impl.hpp"
#endif
#include "faio/detail/runtime/common/io_worker_waker_impl.hpp"
