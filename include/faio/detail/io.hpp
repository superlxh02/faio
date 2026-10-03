#ifndef FAIO_DETAIL_IO_HPP
#define FAIO_DETAIL_IO_HPP
#if defined(_WIN32)
#include "faio/detail/io/platform/windows_framework.hpp"
#else
#include "faio/detail/io/io.hpp"
#endif
#endif // FAIO_DETAIL_IO_HPP