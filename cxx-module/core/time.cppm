module;

#include <faio/time/timer.hpp>

export module faio.time;
export import faio.coroutine;

export namespace faio::time {
using ::faio::time::MissedTickBehavior;
using ::faio::time::interval;
using ::faio::time::interval_at;
using ::faio::time::sleep;
using ::faio::time::sleep_until;
using ::faio::time::timeout;
using ::faio::time::timeout_at;
}  // namespace faio::time
