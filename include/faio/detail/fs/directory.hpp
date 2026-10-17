/**
 * @file faio/detail/fs/directory.hpp
 * @brief 文件与目录异步接口：目录项与异步枚举。
 * @details 文件操作保持共享资源与游标归属；原生异步路径和阻塞服务路径遵循相同完成契约。关闭与析构清理须等待已经接受的借用操作排空。
 */
#pragma once
/** @brief 平台统一的文件系统接口入口。 */
#if defined(_WIN32)
#include "faio/detail/fs/platform/windows/directory.hpp"
#else
#include "faio/detail/fs/platform/unix/directory.hpp"
#endif
