#include <faio/experimental/runtime.h>
#ifdef FAIO_MIXED_USE_EXECUTION
#include <faio/experimental/execution.h>
#endif
#include <faio/faio.hpp>
#include <faio/detail/runtime/context.hpp>
#include <cstddef>

static_assert(__cplusplus > 202302L);
#ifdef ASYNC
#error "runtime.h must not define ASYNC"
#endif
extern "C" std::size_t faio_core_context_size_cxx23();
extern "C" const void* faio_core_default_context_cxx23();

int main() {
    return faio_core_context_size_cxx23() == sizeof(faio::runtime::detail::runtime_context)
        && faio_core_default_context_cxx23() == &faio::runtime::detail::default_service()
        ? 0 : 1;
}
