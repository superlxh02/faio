#ifndef FAIO_DETAIL_EXPERIMENTAL_EXECUTION_DEFAULT_RUNTIME_HPP
#define FAIO_DETAIL_EXPERIMENTAL_EXECUTION_DEFAULT_RUNTIME_HPP
#include "faio/detail/experimental/execution/run.hpp"
#include "faio/detail/runtime/default.hpp"
#include <functional>

namespace faio::experimental {
// The service's shared access lasts through the entire callback. Borrowed
// schedulers/operations must finish within it; shutdown belongs after it.
template <class Function>
decltype(auto) with_default_runtime(Function&& function) {
  if (faio::detail::on_runtime_worker())
    throw std::logic_error("faio experimental: use this_runtime on a runtime worker");
  return faio::runtime::detail::with_default_runtime([&](auto& context) -> decltype(auto) {
    return std::invoke(std::forward<Function>(function), runtime_ref{context});
  });
}
} // namespace faio::experimental
#endif
