#include <faio/experimental/execution.h>
#include <faio/faio.hpp>
#include <exec/task.hpp>
#include <chrono>

namespace ex = faio::experimental;
faio::task<int> native_job(faio::runtime::detail::runtime_context* identity) {
  if (&ex::this_runtime().context() != identity) throw std::logic_error("native host identity");
  co_await faio::time::sleep(std::chrono::milliseconds{1});
  co_return 42;
}
exec::task<int> external_job(ex::runtime_ref runtime) {
  if (&ex::this_runtime().context() != &runtime.context()) throw std::logic_error("external host identity");
  co_return co_await runtime.as_sender(native_job(&runtime.context()));
}
faio::task<bool> entry_like(ex::runtime_ref runtime) {
  if (&ex::this_runtime().context() != &runtime.context()) co_return false;
  for (int iteration = 0; iteration != 32; ++iteration) {
    auto first = runtime.spawn_sender(external_job(runtime));
    auto second = runtime.spawn_sender(stdexec::just(iteration));
    auto a = co_await std::move(first);
    auto b = co_await std::move(second);
    if (!a || std::get<0>(*a) != 42 || !b || std::get<0>(*b) != iteration) co_return false;
    bool duplicate_rejected{};
    try { (void)co_await std::move(first); }
    catch (const std::logic_error&) { duplicate_rejected = true; }
    if (!duplicate_rejected) co_return false;
  }
  auto void_handle = runtime.spawn_sender(stdexec::just());
  if (!(co_await std::move(void_handle))) co_return false;
  auto failed = runtime.spawn_sender(stdexec::then(stdexec::just(), []() -> int { throw std::runtime_error("graph error"); }));
  bool error_observed{};
  try { (void)co_await std::move(failed); }
  catch (const std::runtime_error& error) { error_observed = std::string_view(error.what()) == "graph error"; }
  co_return error_observed;
}
int main() {
  ex::current_thread_runtime current;
  auto options = ex::multi_thread_defaults(); options.workers = 1;
  ex::multi_thread_runtime one{options};
  options.workers = 4;
  ex::multi_thread_runtime multi{options};
  if (!current.ref().context().block_on(entry_like(current.ref()))) return 1;
  if (!one.ref().context().block_on(entry_like(one.ref()))) return 1;
  if (!multi.ref().context().block_on(entry_like(multi.ref()))) return 1;
}
