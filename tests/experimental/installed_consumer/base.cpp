#include <faio/faio.hpp>
static_assert(__cplusplus == 202302L);
#ifdef ASYNC
#error "Base faio include must not define ASYNC"
#endif
int main() { return 0; }
