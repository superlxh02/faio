#ifndef FAIO_DETAIL_SYNC__channel_ptrRECEIVER_HPP
#define FAIO_DETAIL_SYNC__channel_ptrRECEIVER_HPP

#include <memory>

namespace faio::sync::detail {

template <typename Channel> class Receiver {
  using channel_ptr = std::shared_ptr<Channel>;

public:
  Receiver(channel_ptr counter) : _channel_ptr{counter} {}

  ~Receiver() { close(); }

  Receiver(const Receiver &other) : _channel_ptr{other._channel_ptr} {
    _channel_ptr->add_receiver();
  }

  // 拷贝构造函数
  auto operator=(const Receiver &other) -> Receiver & {
    // 先减去当前receiver的引用计数
    _channel_ptr->sub_receiver();
    // 然后赋值为other的channel
    _channel_ptr = other._channel_ptr;
    // 然后增加other的receiver的引用计数
    _channel_ptr->add_receiver();
    return *this;
  }

  Receiver(Receiver &&other) : _channel_ptr{std::move(other._channel_ptr)} {}

  auto operator=(Receiver &&other) -> Receiver & {
    _channel_ptr->sub_receiver();
    _channel_ptr = std::move(other->_channel_ptr);
    return *this;
  }

  auto recv() { return _channel_ptr->recv(); }

  void close() {
    if (_channel_ptr) {
      _channel_ptr->sub_receiver();
      _channel_ptr.reset();
    }
  }

private:
  channel_ptr _channel_ptr;
};
} // namespace faio::sync::detail

#endif // FAIO_DETAIL_SYNC__channel_ptrRECEIVER_HPP