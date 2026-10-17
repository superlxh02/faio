/**
 * @file faio/detail/runtime/common/external_work_host.hpp
 * @brief 运行时调度、线程协调与定时驱动：外部工作宿主接口。
 * @details 运行时按所属线程区分私有状态与跨线程发布状态。任务接纳先登记完成责任，关闭先停止接纳再排空；休眠登记后复查工作以避免丢失唤醒。
 */
#pragma once
#include "faio/detail/runtime/common/external_work.hpp"
