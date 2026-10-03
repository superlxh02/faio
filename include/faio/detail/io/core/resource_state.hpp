#pragma once
#include "faio/detail/io/context.hpp"
#include <atomic>
#include <mutex>

namespace faio::io::detail {
/** @brief 一次发布后不可变的 domain 租约，普通访问只需一次 acquire 布尔读取。
 * @details 首次写入由 resource.binding_mutex 串行；release 发布后，读者才访问
 * shared_ptr。 未发布读者不读取该 shared_ptr，避免跨 worker 首次 bind
 * 的读写数据竞争。
 */
class domain_owner {
 public:
  domain_owner() = default;

  domain_owner(const domain_owner&) = delete;

  domain_owner& operator=(const domain_owner&) = delete;

  domain_owner& operator=(std::shared_ptr<io_domain> value) noexcept {
    value_ = std::move(value);  // 只在局部首次绑定锁内写入一次，此后永不修改。
    published_.store(true, std::memory_order_release);
    return *this;
  }

  explicit operator bool() const noexcept { return published_.load(std::memory_order_acquire); }

  io_domain* get() const noexcept {
    return published_.load(std::memory_order_acquire) ? value_.get() : nullptr;
  }

  io_domain* operator->() const noexcept { return get(); }

  operator std::shared_ptr<io_domain>() const noexcept {
    return published_.load(std::memory_order_acquire) ? value_ : std::shared_ptr<io_domain>{};
  }

 private:
  std::shared_ptr<io_domain> value_;
  std::atomic<bool> published_{};
};

/** @brief 长期资源控制块；业务协程迁移不改变 reactor 归属。 */
struct resource_state {
  domain_owner owner;        ///< 首次归属发布后不可变，租约覆盖所有完成与清理。
  std::mutex binding_mutex;  ///< 仅首次绑定控制面取得，不进入普通 IO 热路径。
  std::atomic<native_descriptor> handle{
      -1};  ///< exchange(-1) 是关闭/导出的唯一接管点，不截断 Win64 SOCKET。
#if defined(_WIN32)
  native_handle_kind native_kind{
      native_handle_kind::windows_socket};  ///< 与原生关闭器一起固定的资源种类。
#endif
  std::atomic<std::size_t> wrappers{0};  ///< 包装对象所有者数量，与操作租约区分。
  std::uint64_t id{};                    ///< 单调资源 token，fd 重用不能命中旧注册事件。
  bool owns_handle{}, regular_file{}, registered{}, closing{}, close_pending{}, write_shutdown{};
  bool write_shutdown_issued{};  ///< 半关闭只提交一次，原生CQE前仍保留active租约。
  std::size_t active{};
  int socket_family{};  ///< 首次归属控制面缓存 SO_DOMAIN，避免每次 CmdSock
                        ///< 查询原生 family。
  std::uint32_t readiness{};
  std::uint64_t readiness_generation{};  ///< 每批事件递增，guard清除不能抹掉更新事件。
  void* read_reserved{};
  void* write_reserved{};
  operation_state* reader{};
  operation_state* writer{};
  operation_state* observers{};
  operation_state* close_waiter{};

  ~resource_state();

  /** @brief 最后 owned write half
   * 析构请求半关闭，单次写及组合写租约均排空后执行。 */
  void request_shutdown_write() noexcept;

  native_descriptor fd() const noexcept { return handle.load(std::memory_order_acquire); }
};
}  // namespace faio::io::detail
