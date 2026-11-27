module;

#include <faio/sync/channel.hpp>
#include <faio/sync/coordination.hpp>
#include <faio/sync/mutex.hpp>

export module faio.sync;
export import faio.coroutine;

export namespace faio::sync {
using ::faio::sync::barrier;
using ::faio::sync::basic_barrier;
using ::faio::sync::basic_condition_variable;
using ::faio::sync::basic_latch;
using ::faio::sync::basic_mutex;
using ::faio::sync::basic_semaphore;
using ::faio::sync::condition_variable;
using ::faio::sync::latch;
using ::faio::sync::mpsc;
using ::faio::sync::mutex;
using ::faio::sync::semaphore;
}  // namespace faio::sync
