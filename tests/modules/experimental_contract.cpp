#include <cstdlib>
#include <stdexcept>

#if FAIO_EXPERIMENTAL_HAS_REFLECTION
#include <meta>
#endif
#if FAIO_EXPERIMENTAL_HAS_EXECUTION
#include <stdexec/execution.hpp>
#include <tuple>
#endif

#if defined(FAIO_USE_HEADERS) && FAIO_USE_HEADERS
#include <faio/experimental/runtime_options.h>
#if FAIO_EXPERIMENTAL_HAS_EXECUTION
#include <faio/experimental/execution.h>
#endif
#if FAIO_EXPERIMENTAL_HAS_REFLECTION
#include <faio/experimental/reflection.h>
#endif
#else
import faio.experimental;
#endif

#if FAIO_EXPERIMENTAL_HAS_REFLECTION
struct[[= faio::experimental::runtime_options{.workers = 3}]] configured {};

static_assert(faio::experimental::runtime_options_of<^^configured>().workers == 3);
#endif

int main() {
  using namespace faio::experimental;
  runtime_options options{.mode = faio::runtime::mode::current_thread, .workers = 1};
  if (validate_options(options) != options_error::none
      || to_runtime_config(options)._num_workers != 1)
    return EXIT_FAILURE;
#if FAIO_EXPERIMENTAL_HAS_EXECUTION
  current_thread_runtime runtime;
  const auto value = runtime.run(stdexec::just(42));
  if (!value || std::get<0>(*value) != 42)
    return EXIT_FAILURE;
#endif
  options.workers = 2;
  return validate_options(options) == options_error::workers ? EXIT_SUCCESS : EXIT_FAILURE;
}
