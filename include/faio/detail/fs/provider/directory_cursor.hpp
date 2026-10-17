/**
 * @file faio/detail/fs/provider/directory_cursor.hpp
 * @brief 文件与目录异步接口：原生目录游标。
 * @details 文件操作保持共享资源与游标归属；原生异步路径和阻塞服务路径遵循相同完成契约。关闭与析构清理须等待已经接受的借用操作排空。
 */
#pragma once
#if defined(_WIN32)
#include "faio/detail/fs/platform/windows/directory_cursor.hpp"
#elif defined(__linux__)
#include "faio/detail/fs/platform/linux/directory_cursor.hpp"
#endif
