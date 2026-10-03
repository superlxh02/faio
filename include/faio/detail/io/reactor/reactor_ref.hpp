#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace faio::io::detail {
/// @brief 后端中立的方向就绪与关闭信息；ERR/EOF 仍须由真实 IO 判断终态。
enum readiness_bits : std::uint32_t {
  readable_bit = 1,
  writable_bit = 2,
  error_bit = 4,
  read_closed_bit = 8,    ///< 读方向 EOF；保留既有读关闭位值，可能仍有未读数据。
  write_closed_bit = 16,  ///< 写方向关闭；不能使单独的 readable 观察立即完成。
  closed_bit = read_closed_bit | write_closed_bit  ///< 聚合信息掩码，任一方向关闭即匹配。
};

/// @brief 内核只保存注册代际标识，不保存协程或 awaiter 地址。
/** @details reactor 完整填写返回 count 内的 key/flags，未使用输出槽无需清零。
 *          调用方只读取有效数量；需要空事件时显式使用 readiness_event{}。
 */
struct readiness_event {
  std::uint64_t key;
  std::uint32_t flags;
};

/// @brief 拥有 reactor 的小函数表；每批 poll 仅一次类型擦除调用。
/// @details attach/detach 由 domain 锁串行化，poll 只允许一个 driver，wake
/// 可跨线程。
class reactor_box {
 public:
  template <class R>
  explicit reactor_box(std::unique_ptr<R> object)
      : object_(object.release()), table_(&table_for<R>) {}  // 仅 box 接管具体类型的唯一所有权。

  reactor_box(reactor_box&& other) noexcept
      : object_(std::exchange(other.object_, nullptr)), table_(other.table_) {}

  reactor_box(const reactor_box&) = delete;
  reactor_box& operator=(const reactor_box&) = delete;

  ~reactor_box() {
    if (object_)
      table_->destroy(object_);
  }

  /// @brief 安装长期资源的 readiness 注册，具体后端只保存数值 key。
  /// @param fd 非阻塞原生描述符，生命周期由 resource_state 管理。
  /// @param key 不重复使用的资源标识，用于拒绝 fd 复用后的迟到内核事件。
  /// @return 零或负 errno；失败没有接管描述符所有权。
  int attach(int fd, std::uint64_t key) noexcept { return table_->attach(object_, fd, key); }

  void detach(int fd) noexcept { table_->detach(object_, fd); }

  /// @brief 一次有界内核事件批次，仅驱动线程调用。
  /// @param events 调用者提供的输出缓冲区；空 span 为立即返回的 no-op。
  /// @param timeout_ms 相对等待时长；nullopt 表示无限，零表示非阻塞轮询。
  /// @return 写入事件数或负 errno；EINTR 返回零，宿主重新计算绝对 deadline。
  int poll(std::span<readiness_event> events, std::optional<int> timeout_ms) noexcept {
    return table_->poll(object_, events, timeout_ms);
  }

  /// @brief 跨线程中断 poll；允许合并多次通知，真实状态保留在 domain 中。
  void wake() const noexcept { table_->wake(object_); }

  const char* name() const noexcept { return table_->name; }

 private:
  struct operations {
    int (*attach)(void*, int, std::uint64_t) noexcept;
    void (*detach)(void*, int) noexcept;
    int (*poll)(void*, std::span<readiness_event>, std::optional<int>) noexcept;
    void (*wake)(void*) noexcept;
    void (*destroy)(void*) noexcept;
    const char* name;
  };
  template <class R>
  static inline const operations table_for{
      +[](void* p, int fd, std::uint64_t key) noexcept {
        return static_cast<R*>(p)->attach(fd, key);
      },
      +[](void* p, int fd) noexcept { static_cast<R*>(p)->detach(fd); },
      +[](void* p, std::span<readiness_event> out, std::optional<int> ms) noexcept {
        return static_cast<R*>(p)->poll(out, ms);
      },
      +[](void* p) noexcept { static_cast<R*>(p)->wake(); },
      +[](void* p) noexcept {
        std::unique_ptr<R> owner{static_cast<R*>(p)};  // 恢复唯一所有权并调用正确类型的析构。
      },
      R::backend_name};
  void* object_{};             ///< 仅此 box 拥有具体 reactor；引用只经表转换。
  const operations* table_{};  ///< 静态表没有动态分配或虚函数层次。
};

reactor_box make_platform_reactor();
}  // namespace faio::io::detail
