/**
 * @file faio/detail/tls/detail/state.hpp
 * @brief 连接的共享状态、稳定缓冲区、方向门闩与终止记录。
 */
#pragma once
#include "faio/detail/coroutine/wait/generation_event.hpp"
#include "faio/detail/tls/detail/bio_engine.hpp"
#include "faio/detail/tls/detail/gate_lease.hpp"
#include "faio/detail/tls/detail/transport.hpp"
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>

namespace faio::tls::detail {
/** @brief 共享 TLS 引擎、异步传输与方向操作状态。 */
template <protocol_transport S>
class tls_state {
 public:
  /**
   * @brief 将传输和现有 BIO 引擎移动到新分配的共享连接状态；不再次验证配置。
   * @param transport 连接取得所有权的已有异步传输。
   * @param engine 已创建的纯内存 TLS 引擎。
   * @return 包含新共享状态的成功结果；分配或构造异常按标准 C++ 规则传播。
   */
  static auto create(S transport, bio_engine engine) -> expected<std::shared_ptr<tls_state>> {
    return std::make_shared<tls_state>(std::move(transport), std::move(engine));
  }

  /**
   * @brief 移入传输和 BIO 引擎，其余缓冲游标、方向门和关闭标志使用成员初始值。
   * @param stream 由当前状态独占拥有的底层传输。
   * @param bio 与该传输配套的纯内存引擎。
   */
  tls_state(S stream, bio_engine bio) : transport(std::move(stream)), engine(std::move(bio)) {}

  /**
   * @brief 检查终止状态并获取对应应用方向的独占门。
   * @param direction 读方向或写方向。
   * @return 自动归还的门租约，忙时返回 EBUSY。
   */
  auto try_enter(app_direction direction) -> expected<gate_lease> {
    std::lock_guard lock(mutex);
    if (failure)
      return std::unexpected{*failure};
    if (closed || closing)
      return std::unexpected{error{static_cast<int>(errc::CLOSED), 0, error_domain::TLS}};
    return gate_lease::try_acquire(direction == app_direction::READ ? read_gate : write_gate,
                                   &changed);
  }

  /**
   * @brief 只选出一个关闭协调者，并停止尚在等待的应用读取。
   * @return 本次取得协调职责时返回 true。
   */
  auto begin_close() -> bool {
    {
      std::lock_guard lock(mutex);
      if (closing || closed || close_result)
        return false;
      closing = true;
    }
    read_close.request_stop();
    changed.notify_all();
    return true;
  }

  /**
   * @brief 保存首个根错误并取消底层传输分支，随后请求底层关闭。
   * @param reason 首次失败的原因，连接根错误进度归零。
   */
  void request_abort(error reason) noexcept {
    {
      std::lock_guard lock(mutex);
      if (closed)
        return;
      if (!failure)
        failure = reason.with_progress(0);
      terminal.store(true, std::memory_order_release);
    }
    abort_stop.request_stop();
    if constexpr (requires(S& value) {
                    { value.abort() } noexcept;
                  }) {
      transport.abort();
    } else if constexpr (requires(S& value) {
                           value.resource()->owner->request_close(value.resource());
                         }) {
      const auto& resource = transport.resource();
      if (resource && resource->owner)
        resource->owner->request_close(resource);
    }
    changed.notify_all();
  }

  /**
   * @brief 以 acquire 读取连接终止标志；本类型不检查包装器是否已被移动。
   * @return request_abort 或成功 close 已发布终止状态时为 true。
   */
  auto is_terminated() const noexcept -> bool { return terminal.load(std::memory_order_acquire); }

  /**
   * @brief 复制协商结果和对端证书 DER，避免返回对 SSL 内部数据的借用。
   * @return 拥有型连接信息快照。
   */
  auto connection_info() const -> ::faio::tls::connection_info {
    std::lock_guard lock(mutex);
    return engine.connection_info();
  }

  S transport;                              /**< 由连接状态拥有的底层异步传输。 */
  bio_engine engine;                        /**< 必须在 mutex 下串行访问的纯内存引擎。 */
  mutable std::mutex mutex;                 /**< 保护 SSL 调用、明密文游标和终止记录。 */
  ::faio::detail::generation_event changed; /**< 广播状态变化，防止检查与挂起之间丢通知。 */
  std::atomic_bool read_gate{}, write_gate{}, wire_read_gate{},
      wire_write_gate{};                      /**< 底层物理写独占门。 */
  std::atomic_bool terminal{};                /**< 供 is_terminated 读取的终止快照。 */
  std::stop_source abort_stop, read_close;    /**< 本地关闭时停止应用读取。 */
  std::array<char, 16 * 1024> wire_output{};  /**< 跨短写及取消保持稳定的密文输出。 */
  std::size_t wire_size{}, wire_offset{};     /**< 已向底层真实提交的密文前缀。 */
  std::array<char, 32 * 1024> plain_read{};   /**< 解密暂存区，不直接借用用户缓冲。 */
  std::size_t plain_size{}, plain_offset{};   /**< 已交付用户的明文前缀。 */
  std::array<char, 16 * 1024> plain_write{};  /**< SSL 写重试使用的稳定明文副本。 */
  std::size_t write_size{};                   /**< 本次写重试的固定明文长度。 */
  std::optional<error> failure;               /**< 只保存第一次终止错误，密文进度归零。 */
  std::optional<expected<void>> close_result; /**< 供重复 close 调用共享的最终结果。 */
  bool local_shutdown{}, peer_shutdown{}, closing{}, closed{},
      transport_eof{}; /**< 底层是否已经返回 EOF。 */
};
}  // namespace faio::tls::detail
