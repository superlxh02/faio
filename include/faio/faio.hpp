/**
 * @file faio/faio.hpp
 * @brief faio 稳定功能的总入口，汇集按模块组织的公开头文件。
 */
#ifndef FAIO_FAIO_HPP
#define FAIO_FAIO_HPP

#include "faio/common/error.hpp"          /* IWYU pragma: export */
#include "faio/coroutine/awaiter.hpp"     /* IWYU pragma: export */
#include "faio/coroutine/composition.hpp" /* IWYU pragma: export */
#include "faio/coroutine/lifecycle.hpp"   /* IWYU pragma: export */
#include "faio/coroutine/sequence.hpp"    /* IWYU pragma: export */
#include "faio/coroutine/task.hpp"        /* IWYU pragma: export */
#include "faio/fs/directory.hpp"          /* IWYU pragma: export */
#include "faio/fs/file.hpp"               /* IWYU pragma: export */
#include "faio/fs/path.hpp"               /* IWYU pragma: export */
#include "faio/io/native.hpp"             /* IWYU pragma: export */
#include "faio/io/stream.hpp"             /* IWYU pragma: export */
#include "faio/log/logger.hpp"            /* IWYU pragma: export */
#include "faio/net/address.hpp"           /* IWYU pragma: export */
#include "faio/net/local.hpp"             /* IWYU pragma: export */
#include "faio/net/tcp.hpp"               /* IWYU pragma: export */
#include "faio/net/udp.hpp"               /* IWYU pragma: export */
#include "faio/runtime/default.hpp"       /* IWYU pragma: export */
#include "faio/runtime/instance.hpp"      /* IWYU pragma: export */
#include "faio/sync/channel.hpp"          /* IWYU pragma: export */
#include "faio/sync/coordination.hpp"     /* IWYU pragma: export */
#include "faio/sync/mutex.hpp"            /* IWYU pragma: export */
#include "faio/time/timer.hpp"            /* IWYU pragma: export */

#if defined(FAIO_ENABLE_PROCESS) && FAIO_ENABLE_PROCESS
#include "faio/process/child.hpp"   /* IWYU pragma: export */
#include "faio/process/command.hpp" /* IWYU pragma: export */
#endif

#if defined(FAIO_ENABLE_TLS) && FAIO_ENABLE_TLS
#include "faio/tls/handshake.hpp" /* IWYU pragma: export */
#include "faio/tls/stream.hpp"    /* IWYU pragma: export */
#endif

#endif
