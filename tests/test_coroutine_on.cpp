/** @brief 比较来源和目标运行时是否相同，并验证两个方向的关闭行为。 */
#include <faio/runtime/instance.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {
using faio::anchored_runtime;
using faio::basic_runtime_anchor;
using faio::on;
using faio::task;

void verify(bool value) {
  if (!value)
    throw std::logic_error("F07 coroutine assertion failed");
}

task<std::unique_ptr<int>> read_on(faio::scheduler_ref source) {
  verify((co_await faio::this_coro::scheduler()) != source);
  co_return std::make_unique<int>(41);
}

task<void> throws() {
  throw std::runtime_error("target error");
  co_return;
}

task<int> chain(basic_runtime_anchor<> anchor) {
  auto source = co_await faio::this_coro::scheduler();
  auto result = co_await on(anchor, read_on(source));
  verify((co_await faio::this_coro::scheduler()) == source);
  try {
    co_await on(anchor, throws());
    ADD_FAILURE() << "expected cross-runtime failure";
  } catch (const std::runtime_error& error) {
    verify(std::string_view(error.what()) == "target error");
  }
  verify((co_await faio::this_coro::scheduler()) == source);
  co_return *result;
}

task<void> require_same(faio::scheduler_ref selected) {
  verify((co_await faio::this_coro::scheduler()) == selected);
}

task<void> same(basic_runtime_anchor<> handle) {
  auto selected = co_await faio::this_coro::scheduler();
  co_await on(handle, require_same(selected));
}

task<void> restricted(basic_runtime_anchor<> anchor) {
  auto selected = co_await faio::this_coro::scheduler();
  try {
    co_await on(anchor, require_same(selected));
    ADD_FAILURE() << "expected cross-runtime failure";
  } catch (const std::logic_error&) {
  }
  verify((co_await faio::this_coro::scheduler()) == selected);
}

task<void> not_started(bool& started) {
  started = true;
  co_return;
}

task<void> reject(basic_runtime_anchor<> handle, bool& started) {
  try {
    co_await on(handle, not_started(started));
    ADD_FAILURE() << "expected cross-runtime failure";
  } catch (const std::logic_error&) {
    verify(!started);
  }
}

task<bool> wait_for_stop(std::atomic<bool>& armed) {
  const auto token = co_await faio::this_coro::stop_token();
  armed.store(true, std::memory_order_release);
  armed.notify_all();
  while (!token.stop_requested())
    co_await faio::this_coro::yield();
  co_return true;
}

task<bool> forward_stop(basic_runtime_anchor<> handle, std::atomic<bool>& armed) {
  co_return co_await on(handle, wait_for_stop(armed));
}

void ready(std::atomic<bool>& value) {
  value.wait(false, std::memory_order_acquire);
}

TEST(CoroutineOn, TransferAndStop) {
  const auto cfg = faio::config_builder{}.set_num_workers(2).build();
  faio::runtime::current_thread_runtime current;
  anchored_runtime target{cfg};
  verify(current.block_on(chain(target.anchor())) == 41);
  verify(current.block_on(chain(target.anchor())) == 41);
  target.block_on(same(target.anchor()));
  target.block_on(std::move(same(target.anchor())).with_no_stealing());
  current.block_on(std::move(restricted(target.anchor())).with_no_stealing());
  bool started{};
  basic_runtime_anchor<> expired;
  {
    anchored_runtime scoped{cfg};
    expired = scoped.anchor();
  }
  current.block_on(reject(expired, started));
  std::atomic<bool> armed{};
  std::jthread cancel_target{[&] {
    ready(armed);
    target.stop(faio::io::shutdown_policy::CANCEL_ALL);
  }};
  verify(current.block_on(forward_stop(target.anchor(), armed)));
  cancel_target.join();
  current.block_on(reject(target.anchor(), started));

  anchored_runtime another{cfg};
  faio::runtime::multi_thread_runtime source{cfg};
  std::atomic<bool> source_armed{};
  std::jthread cancel_source{[&] {
    ready(source_armed);
    source.stop(faio::io::shutdown_policy::CANCEL_ALL);
  }};
  verify(source.block_on(forward_stop(another.anchor(), source_armed)));
  cancel_source.join();
}
}  // namespace
