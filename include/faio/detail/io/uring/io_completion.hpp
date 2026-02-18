#ifndef FAIO_DETAIL_IO_URING_COMPLETION_HPP
#define FAIO_DETAIL_IO_URING_COMPLETION_HPP

#include <liburing.h>
namespace faio::io::detail {
struct io_user_data_t;
}

namespace faio::io::detail {
struct io_completion_t {

  int expected() { return cqe->res; }

  detail::io_user_data_t *data() {
    return reinterpret_cast<detail::io_user_data_t *>(cqe->user_data);
  }

  io_uring_cqe *cqe;
};
} // namespace faio::io::detail
#endif // FAIO_DETAIL_IO_URING_COMPLETION_HPP