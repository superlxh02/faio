/**
 * @file faio/detail/tls/detail/transport.hpp
 * @brief TLS 使用的异步传输约束和关闭、刷新适配。
 */
#pragma once
#include "faio/detail/coroutine/task.hpp"
#include "faio/detail/io/traits/async_read.hpp"
#include "faio/detail/io/traits/async_write.hpp"
#include <concepts>

namespace faio::tls::detail {
/** @brief TLS 底层传输必须可移动，支持借用式异步读写以及异步关闭。 */
template <class S>
concept protocol_transport = std::movable<S> && io::borrowed_async_reader<S>
                             && io::borrowed_async_writer<S> && requires(S& stream) {
                                  { stream.close() } -> io::io_awaitable_of<void>;
                                };

/**
 * @brief 刷新提供 flush 的传输；无用户态写缓存的传输直接成功。
 * @tparam S 满足 protocol_transport 的传输类型。
 * @param stream 调用期间保持有效的底层传输。
 * @return 底层刷新结果；未提供 flush 时返回成功。
 */
template <protocol_transport S>
auto transport_flush(S& stream) -> task<expected<void>> {
  if constexpr (requires {
                  { stream.flush() } -> io::io_awaitable_of<void>;
                })
    co_return co_await stream.flush();
  else
    co_return expected<void>{};
}

/**
 * @brief 通过传输自己的异步接口关闭连接并等待关闭完成。
 * @tparam S 满足 protocol_transport 的传输类型。
 * @param stream 待关闭的底层传输。
 * @return 底层关闭操作的结果。
 */
template <protocol_transport S>
auto transport_close(S& stream) -> task<expected<void>> {
  co_return co_await stream.close();
}
}  // namespace faio::tls::detail
