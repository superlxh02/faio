#ifndef FAIO_TEST_MULTIPLE_VALUE_SENDER_HPP
#define FAIO_TEST_MULTIPLE_VALUE_SENDER_HPP
#include <stdexec/execution.hpp>

struct multiple_value_sender {
  using sender_concept = stdexec::sender_tag;
  template <class Self, class Environment = stdexec::env<>>
  static consteval auto get_completion_signatures() noexcept {
    return stdexec::completion_signatures<stdexec::set_value_t(int), stdexec::set_value_t(double)>{};
  }
  template <class Receiver> struct operation {
    using operation_state_concept = stdexec::operation_state_tag;
    Receiver receiver;
    void start() noexcept { stdexec::set_value(std::move(receiver), 42); }
  };
  template <stdexec::receiver Receiver> auto connect(Receiver receiver) const -> operation<Receiver> {
    return {std::move(receiver)};
  }
};
#endif
