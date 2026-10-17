/**
 * @file faio/detail/io/core/resource_state.hpp
 * @brief 跨平台异步 IO：长期资源控制块。
 * @details 核心层维护长期资源身份、稳定操作代际和生命周期。资源首次绑定后保持所属域，关闭、取消、完成与槽位复用必须遵循同一排空协议。
 */
#pragma once
#include "faio/detail/io/core/context.hpp"
#include "faio/detail/io/protocol/backend.hpp"
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
    /// @brief 只在局部首次绑定锁内写入一次，此后永不修改。
    value_ = std::move(value);
    published_.store(true, std::memory_order_release);
    return *this;
  }

  explicit operator bool() const noexcept { return published_.load(std::memory_order_acquire); }

  /// @brief 取得当前对象保存的值或借用句柄，是否转交所有权由返回类型决定。
  io_domain* get() const noexcept {
    return published_.load(std::memory_order_acquire) ? value_.get() : nullptr;
  }

  io_domain* operator->() const noexcept { return get(); }

  operator std::shared_ptr<io_domain>() const noexcept {
    return published_.load(std::memory_order_acquire) ? value_ : std::shared_ptr<io_domain>{};
  }

 private:
  std::shared_ptr<io_domain> value_;  ///< 首次绑定后不可变的 IO 域共享租约，发布后才允许读取。
  std::atomic<bool> published_{};  ///< 一次发布的归属状态；acquire 成功后才允许读取共享域租约。
};

/** @brief 长期资源控制块；业务协程迁移不改变 reactor 归属。 */
struct resource_state {
  domain_owner owner;  ///< 首次归属发布后不可变，租约覆盖所有完成与清理。
  std::mutex binding_mutex;  ///< 仅首次绑定控制面取得，不进入普通 IO 热路径。
  std::atomic<native_descriptor> handle{
      -1};  ///< exchange(-1) 是关闭/导出的唯一接管点，不截断 Win64 SOCKET。
#if defined(_WIN32)
  native_handle_kind native_kind{
      native_handle_kind::windows_socket};  ///< 与原生关闭器一起固定的资源种类，文件请求显式使用文件句柄标签。
#endif
  std::atomic<std::size_t> wrappers{0};  ///< 包装对象所有者数量，与操作租约区分。
  std::uint64_t id{};  ///< 单调资源 token，fd 重用不能命中旧注册事件。
  bool owns_handle{},  ///< 该资源是否承担关闭原生句柄的所有权。
      regular_file{},  ///< 资源是否为普通文件，用于选择文件原生或阻塞服务路径。
      registered{},  ///< 业务句柄是否已经登记到后端就绪队列。
      closing{},  ///< 资源已经进入关闭阶段，拒绝新的业务操作。
      close_pending{},  ///< 是否已有等待活跃 IO 排空的关闭请求。
      write_shutdown{};  ///< 是否已经请求写半关闭，阻止后续写入。
  bool write_shutdown_issued{};  ///< 半关闭只提交一次，原生CQE前仍保留active租约。
  std::size_t active{};  ///< 资源上尚未安全完成的活跃操作数量，归零后才可关闭。
  int socket_family{};  ///< 首次归属控制面缓存 SO_DOMAIN，避免每次 CmdSock 查询原生 family。
  std::uint32_t readiness{};  ///< 当前资源的粘性就绪提示，真实暂不可用结果或同代际守卫才可清除对应方向。
  std::uint64_t readiness_generation{};  ///< 每批事件递增，guard清除不能抹掉更新事件。
  void* read_reserved{};  ///< 组合读操作的独占身份，非空时其他读操作必须拒绝。
  void* write_reserved{};  ///< 组合写操作的独占身份，非空时其他写操作必须拒绝。
  operation_state* reader{};  ///< 当前持有读方向执行权的稳定操作节点。
  operation_state* writer{};  ///< 当前持有写方向执行权的稳定操作节点。
  operation_state* observers{};  ///< 资源上的就绪观察者链表，不取得数据消费权。
  operation_state* close_waiter{};  ///< 等待该资源全部活跃 IO 排空的关闭操作节点。

  ~resource_state();

  /** @brief 最后 owned write half
   * 析构请求半关闭，单次写及组合写租约均排空后执行。 */
  void request_shutdown_write() noexcept;

  /// @brief 读取原生句柄值，不在此接口转移关闭责任。
  native_descriptor fd() const noexcept { return handle.load(std::memory_order_acquire); }
};
}
