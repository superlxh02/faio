#include "toolchain_task_std_stop_token_environment.hpp"

int main() {
  std::stop_source source;
  auto operation = stdexec::connect(
      stdexec::starts_on(stdexec::inline_scheduler{}, std_stop_token_task()),
      std_stop_token_receiver{{source.get_token()}});
  stdexec::start(operation);
}
