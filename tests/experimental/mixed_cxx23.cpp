#include <faio/faio.hpp>
#include <faio/detail/runtime/context.hpp>
#include <faio/detail/runtime/common/external_work_host.hpp>
#include <cstddef>

static_assert(__cplusplus == 202302L);
extern "C" std::size_t faio_core_context_size_cxx23() {
    return sizeof(faio::runtime::detail::runtime_context);
}
extern "C" const void* faio_core_default_context_cxx23() {
    return &faio::runtime::detail::default_service();
}
