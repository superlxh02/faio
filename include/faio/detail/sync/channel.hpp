#ifndef FAIO_DETAIL_SYNC_CHANNEL_HPP
#define FAIO_DETAIL_SYNC_CHANNEL_HPP

#include "faio/detail/sync/channel/channel.hpp"
#include "faio/detail/sync/channel/receiver.hpp"
#include "faio/detail/sync/channel/sender.hpp"
namespace faio::sync {
template <typename T> class channel {
  using ChannelImpl = detail::Channel<T>;

public:
  using Sender = detail::Sender<ChannelImpl>;
  using Receiver = detail::Receiver<ChannelImpl>;

public:
  // 创建一个通道，并返回一个Sender和Receiver
  [[nodiscard]]
  static auto make(std::size_t max_cap) -> std::pair<Sender, Receiver> {
    auto counter = std::make_shared<ChannelImpl>(1, 1, max_cap);
    return std::make_pair(Sender{counter}, Receiver{counter});
  }
};
} // namespace faio::sync
#endif // FAIO_DETAIL_SYNC_CHANNEL_HPP
