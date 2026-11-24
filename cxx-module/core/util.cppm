module;

#define FAIO_DETAIL_SEPARATE_DUPLEX 1
#include <faio/detail/common/static_math.hpp>
#include <faio/detail/common/util/cpu_relax.hpp>

export module faio.util;
export import faio.common;

export namespace faio::util {
using ::faio::util::cpu_relax;
using ::faio::util::static_log;
using ::faio::util::static_pow;
}  // namespace faio::util
