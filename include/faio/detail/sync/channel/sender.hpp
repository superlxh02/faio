#ifndef FAIO_DETAIL_SYNC__channel_ptrSENDER_HPP
#define FAIO_DETAIL_SYNC__channel_ptrSENDER_HPP
#include <memory>

namespace faio::sync::detail {

template <typename Channel> class Sender {
  using _channel_ptrptr = std::shared_ptr<Channel>;

public:
  Sender(_channel_ptrptr counter) : _channel_ptr{counter} {}

  ~Sender() { close(); }

  Sender(const Sender &other) : _channel_ptr{other._channel_ptr} {
    _channel_ptr->add_sender();
  }

  auto operator=(const Sender &other) -> Sender & {
    _channel_ptr->sub_sender();
    _channel_ptr = other._channel_ptr;
    _channel_ptr->add_sender();
    return *this;
  }

  Sender(Sender &&other) : _channel_ptr{std::move(other._channel_ptr)} {}

  auto operator=(Sender &&other) -> Sender & {
    _channel_ptr->sub_sender();
    _channel_ptr = std::move(other._channel_ptr);
    return *this;
  }

  auto send(Channel::ValueType value) {
    return _channel_ptr->send(std::move(value));
  }

  void close() {
    if (_channel_ptr) {
      _channel_ptr->sub_sender();
      _channel_ptr.reset();
    }
  }

private:
  _channel_ptrptr _channel_ptr;
};

} // namespace faio::sync::detail

#endif // FAIO_DETAIL_SYNC__channel_ptrSENDER_HPP