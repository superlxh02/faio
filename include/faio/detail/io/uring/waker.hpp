#ifndef FAIO_DETAIL_IO_URING_WEAKER_HPP
#define FAIO_DETAIL_IO_URING_WEAKER_HPP
#include "faio/detail/io/uring/io_uring.hpp"
#include "fastlog/fastlog.hpp"
#include <cstdint>
#include <cstring>
#include <liburing.h>
#include <sys/eventfd.h>
#include <unistd.h>
namespace faio::io::detail {

/// 唤醒器，用于唤醒协程
class Waker {
public:
  Waker() : _fd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {}

  ~Waker() { ::close(_fd); }

public:
  /// 唤醒
  void wake_up() {
    static constexpr std::uint64_t buf{1};
    if (auto res = ::write(this->_fd, &buf, sizeof(buf)); res < 0) {
      // EAGAIN 是正常的：eventfd 计数器溢出，说明已经有未消费的唤醒
      if (errno != EAGAIN) {
        fastlog::console.error("wake_up failed: {}", strerror(errno));
      }
    }
  }
  // 开始监视,用于开始监视唤醒
  void start_watch() {
    if (_flag != 0) {
      _flag = 0;
      auto sqe = current_uring->get_sqe();
      if (sqe == nullptr) {
        fastlog::console.error("get sqe failed");
        return;
      }
      io_uring_prep_read(sqe, _fd, &_flag, sizeof(_flag), 0);
      io_uring_sqe_set_data(sqe, nullptr);
    }
  }

private:
  std::uint64_t _flag{1};
  int _fd;
};

} // namespace faio::io::detail
#endif // FAIO_DETAIL_IO_URING_WEAKER_HPP