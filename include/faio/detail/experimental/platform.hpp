#ifndef FAIO_DETAIL_EXPERIMENTAL_PLATFORM_HPP
#define FAIO_DETAIL_EXPERIMENTAL_PLATFORM_HPP

#include <version>

#if !defined(__linux__)
#error "faio experimental: Linux is required"
#endif
#if defined(__clang__) || !defined(__GNUC__) || __GNUC__ != 16 || __GNUC_MINOR__ != 1
#error "faio experimental: GCC 16.1.x is required"
#endif
#if !defined(_GLIBCXX_RELEASE) || _GLIBCXX_RELEASE != 16
#error "faio experimental: matching libstdc++ 16 is required"
#endif
#if __cplusplus <= 202302L
#error "faio experimental: C++26 is required"
#endif

#ifndef FAIO_EXPERIMENTAL_HAS_REFLECTION
#define FAIO_EXPERIMENTAL_HAS_REFLECTION 0
#endif
#ifndef FAIO_EXPERIMENTAL_HAS_EXECUTION
#define FAIO_EXPERIMENTAL_HAS_EXECUTION 0
#endif

#endif
