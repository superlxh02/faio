#ifndef FAIO_IMPORT_MODULE
#define FAIO_IMPORT_MODULE faio
#endif

import FAIO_IMPORT_MODULE;

namespace {

struct completed_coroutine {
  struct promise_type {
    completed_coroutine get_return_object() noexcept { return {}; }
    std::suspend_never initial_suspend() noexcept { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() noexcept {}
    void unhandled_exception() noexcept {}
  };
};

completed_coroutine check_coroutine(int& result) {
  co_await std::suspend_never{};
  result = 42;
  co_return;
}

static_assert(sizeof(std::coroutine_handle<>) > 0);

#if defined(FAIO_IMPORT_HAS_TASK)
faio::task<int> value() {
  co_return 42;
}

faio::task<int> nested() {
  const auto [left, right] = co_await faio::join(value(), value());
  co_return (left + right) / 2;
}
#endif

}  // namespace

int main() {
  int result{};
  check_coroutine(result);
  if (result != 42)
    return 1;

#if defined(FAIO_IMPORT_HAS_RUNTIME)
  if (faio::block_on(nested()) != 42)
    return 1;
#elif defined(FAIO_IMPORT_HAS_TASK)
  const auto task = nested();
#endif

#if defined(FAIO_IMPORT_COMMON)
  const faio::expected<int> expected{42};
  if (!expected || *expected != 42)
    return 1;
#elif defined(FAIO_IMPORT_IO)
  const faio::io::MemoryStream stream;
  if (!stream.bytes().empty())
    return 1;
#elif defined(FAIO_IMPORT_NET)
  const auto address = faio::net::SocketAddr::parse("127.0.0.1", 42);
  if (!address || address->port() != 42)
    return 1;
#elif defined(FAIO_IMPORT_FS)
  const faio::fs::path path{"."};
  if (path.empty())
    return 1;
#elif defined(FAIO_IMPORT_SYNC)
  faio::sync::mutex mutex;
  if (!mutex.try_lock())
    return 1;
  mutex.unlock();
#elif defined(FAIO_IMPORT_TIME)
  static_assert(faio::time::MissedTickBehavior::Burst != faio::time::MissedTickBehavior::Skip);
#elif defined(FAIO_IMPORT_LOG)
  if (!faio::log::logger())
    return 1;
#elif defined(FAIO_IMPORT_BLOCKING)
  faio::blocking::service service{1, 1, 1, faio::blocking::startup_policy::on_demand};
#elif defined(FAIO_IMPORT_UTIL)
  static_assert(faio::util::static_pow(2, 3) == 8);
#elif defined(FAIO_IMPORT_TLS)
  if (!faio::tls::ServerName::parse("localhost"))
    return 1;
#elif defined(FAIO_IMPORT_PROCESS)
  const faio::process::Command command{"unused-command"};
#endif
}
