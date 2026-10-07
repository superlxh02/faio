#ifndef FAIO_DETAIL_EXPERIMENTAL_RUNTIME_FACADE_HPP
#define FAIO_DETAIL_EXPERIMENTAL_RUNTIME_FACADE_HPP

#include "faio/detail/experimental/runtime_options.hpp"
#include "faio/detail/runtime/context.hpp"
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace faio::experimental {
template <faio::runtime::mode Mode> class execution_scheduler;
using current_thread_scheduler = execution_scheduler<faio::runtime::mode::current_thread>;
using multi_thread_scheduler = execution_scheduler<faio::runtime::mode::multi_thread>;
template <class Tuple> class sender_join_handle;

class runtime_ref {
 public:
  explicit runtime_ref(faio::runtime::detail::runtime_context& context) noexcept : context_(&context) {}
  auto mode() const noexcept -> faio::runtime::mode { return context_->config()._mode; }
  auto io_capabilities() const noexcept { return context_->io_capabilities(); }
  auto get_current_thread_scheduler() const -> current_thread_scheduler;
  auto get_multi_thread_scheduler() const -> multi_thread_scheduler;
  template <class Sender> auto run(Sender&& sender) const;
  template <class Sender> auto bind(Sender&& sender) const;
  template <class Sender> auto spawn_sender(Sender&& sender) const;
  template <class T> auto as_sender(faio::task<T> child) const;
  auto context() const noexcept -> faio::runtime::detail::runtime_context& { return *context_; }
 private:
  faio::runtime::detail::runtime_context* context_;
};

namespace detail {
template <faio::runtime::mode Mode>
class runtime_owner {
 public:
  explicit runtime_owner(runtime_options options) : context_(checked_config(options)) {}
  runtime_owner(const runtime_owner&) = delete;
  runtime_owner& operator=(const runtime_owner&) = delete;
  runtime_owner(runtime_owner&&) = delete;
  runtime_owner& operator=(runtime_owner&&) = delete;
  ~runtime_owner() noexcept {
    try { context_.shutdown(faio::io::shutdown_policy::cancel_all); }
    catch (...) {
      std::fputs("faio experimental: owner destroyed with active reservation or from worker\n", stderr);
      std::terminate();
    }
  }
  auto ref() noexcept -> runtime_ref { return runtime_ref{context_}; }
  auto get_scheduler() noexcept -> execution_scheduler<Mode>;
  template <class Sender> auto run(Sender&& sender) { return ref().run(std::forward<Sender>(sender)); }
  template <class Sender> auto bind(Sender&& sender) { return ref().bind(std::forward<Sender>(sender)); }
  template <class Sender> auto spawn_sender(Sender&& sender) { return ref().spawn_sender(std::forward<Sender>(sender)); }
  template <class T> auto as_sender(faio::task<T> child) { return ref().as_sender(std::move(child)); }
  void request_stop() noexcept { context_.request_stop(); }
  void shutdown(faio::io::shutdown_policy policy = faio::io::shutdown_policy::cancel_all) { context_.shutdown(policy); }
  auto io_capabilities() const noexcept { return context_.io_capabilities(); }
 private:
  static auto checked_config(runtime_options options) -> faio::runtime::config {
    if (options.mode != Mode)
      throw std::invalid_argument("faio experimental: runtime owner mode mismatch");
    return to_runtime_config(options);
  }
  faio::runtime::detail::runtime_context context_;
};
} // namespace detail

class current_thread_runtime final : public detail::runtime_owner<faio::runtime::mode::current_thread> {
 public:
  explicit current_thread_runtime(runtime_options options = current_thread_defaults())
      : runtime_owner(options) {}
};
class multi_thread_runtime final : public detail::runtime_owner<faio::runtime::mode::multi_thread> {
 public:
  explicit multi_thread_runtime(runtime_options options = multi_thread_defaults())
      : runtime_owner(options) {}
};

auto this_runtime() -> runtime_ref;
} // namespace faio::experimental
#endif
