// Apply the same toolchain contract as cmake/FaioExperimentalToolchain.cmake.
#include <version>
#if !defined(__linux__) || defined(__clang__) || !defined(__GNUC__) || __GNUC__ != 16 || __GNUC_MINOR__ != 1
#error "faio experimental requires Linux and GCC 16.1.x"
#endif
#if !defined(_GLIBCXX_RELEASE) || _GLIBCXX_RELEASE != 16 || __cplusplus <= 202302L
#error "faio experimental requires C++26 and the matching GCC 16 libstdc++"
#endif

#if defined(FAIO_EXPERIMENTAL_HAS_REFLECTION)
#include <meta>

namespace {
struct options {
  int value;
};
[[= options{42}]] constexpr int annotated() { return 42; }
constexpr auto metadata = std::meta::annotations_of(^^annotated).front();
static_assert(std::meta::extract<options>(metadata).value == [:^^annotated:]());
}  // namespace
#endif
