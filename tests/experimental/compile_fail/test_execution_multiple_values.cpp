#include <faio/experimental/execution.h>
#include "../multiple_value_sender.hpp"
int main() {
  faio::experimental::current_thread_runtime runtime;
  (void)runtime.run(multiple_value_sender{});
}
