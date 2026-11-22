/**
 * @file faio/detail/io/stream/duplex.inc
 * @brief 内存双工通道的读写协程，共享于头文件与具名模块。
 */

/** @brief 从内存双工通道读取字节。 */
FAIO_DETAIL_DUPLEX_INLINE task<expected<std::size_t>> duplex_read(std::shared_ptr<duplex_lane> lane,
                                                                 std::span<char> bytes) {
  if (!lane)
    co_return std::unexpected{make_error(EBADF)};
  if (bytes.empty())
    co_return std::size_t{0};
  try {
    auto guard = co_await lane->mutex.scoped_lock();
    co_await lane->changed.wait(lane->mutex,
                                [&] { return lane->size || lane->writer_closed.load(); });
    const auto count = std::min(bytes.size(), lane->size);
    for (std::size_t i = 0; i < count; ++i)
      bytes[i] = lane->buffer[(lane->head + i) % lane->buffer.size()];
    lane->head = (lane->head + count) % lane->buffer.size();
    lane->size -= count;
    lane->changed.notify_all();
    co_return count;
  } catch (const operation_cancelled&) {
    co_return std::unexpected{make_error(ECANCELED)};
  }
}

/** @brief 向内存双工通道写入字节。 */
FAIO_DETAIL_DUPLEX_INLINE task<expected<std::size_t>> duplex_write(std::shared_ptr<duplex_lane> lane,
                                                                  std::span<const char> bytes) {
  if (!lane)
    co_return std::unexpected{make_error(EBADF)};
  if (bytes.empty())
    co_return std::size_t{0};
  try {
    auto guard = co_await lane->mutex.scoped_lock();
    co_await lane->changed.wait(lane->mutex, [&] {
      return lane->size < lane->buffer.size() || lane->reader_closed.load()
             || lane->writer_closed.load();
    });
    if (lane->reader_closed.load() || lane->writer_closed.load())
      co_return std::unexpected{make_error(EPIPE)};
    const auto count = std::min(bytes.size(), lane->buffer.size() - lane->size);
    for (std::size_t i = 0; i < count; ++i)
      lane->buffer[(lane->head + lane->size + i) % lane->buffer.size()] = bytes[i];
    lane->size += count;
    lane->changed.notify_all();
    co_return count;
  } catch (const operation_cancelled&) {
    co_return std::unexpected{make_error(ECANCELED)};
  }
}
