// Compile the shared coroutine bodies separately for named-module consumers.
#define FAIO_DETAIL_SEPARATE_DUPLEX 1
#include <faio/detail/io/stream/memory_stream.hpp>

namespace faio::io::detail {
#define FAIO_DETAIL_DUPLEX_INLINE
#include <faio/detail/io/stream/duplex.inc>
#undef FAIO_DETAIL_DUPLEX_INLINE
}  // namespace faio::io::detail
