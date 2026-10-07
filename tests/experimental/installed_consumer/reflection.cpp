#include <faio/experimental/reflection.h>
struct [[=faio::experimental::runtime_options{.workers = 3}]] entity {};
static_assert(faio::experimental::runtime_options_of<^^entity>().workers == 3);
int main() { return 0; }
