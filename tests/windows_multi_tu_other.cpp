#include "faio/faio.hpp"
#include "windows_multi_tu_support.hpp"
thread_local std::string windows_tls_second = windows_tls_value("second");

bool windows_second_observes_stop() {
  return faio::detail::current_stop_token.stop_requested();
}

namespace {
faio::task<int> second_task() {
  co_return 42;
}
}  // namespace

int windows_second_task_value() {
  faio::runtime::detail::runtime_context runtime{
      faio::config_builder{}.set_mode(faio::runtime::mode::current_thread).build()};
  return runtime.block_on(second_task());
}
