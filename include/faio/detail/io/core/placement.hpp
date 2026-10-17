/**
 * @file faio/detail/io/core/placement.hpp
 * @brief 跨平台异步 IO：资源所属域选择。
 * @details 核心层维护长期资源身份、稳定操作代际和生命周期。资源首次绑定后保持所属域，关闭、取消、完成与槽位复用必须遵循同一排空协议。
 */
#pragma once
#include "faio/detail/io/core/fwd.hpp"
#include "faio/detail/io/platform/native_types.hpp"
#include <cstddef>
#include <mutex>
#include <vector>

namespace faio::io::detail {
/** @brief runtime 控制面共享的弱 shard 注册表，用于首次分配与 raw fd 兼容查找。
 * @details weak_ptr 不使 domain 与 runtime 形成强引用环；选择在短锁内轮转，
 *          普通已注册 IO 热路径不访问此表，也不迁移资源。
 */
class io_placement_group {
 public:
  explicit io_placement_group(std::size_t count) : domains_(count) {}

  /// @brief 把 IO 域加入放置服务的弱引用表，不延长已退出域的生命期。
  void register_domain(std::size_t index, const std::shared_ptr<io_domain>& domain) noexcept {
    /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
    std::lock_guard lock(mutex_);
    /// @brief 启动屏障前登记，停止后弱租约不会访问 worker 地址。
    domains_[index] = domain;
  }

  std::shared_ptr<io_domain> select() noexcept;

  /** @brief 在同一 runtime 内查找已有 fd 身份，不建立新的资源或原生请求。
   * @param fd 调用方仍持有的原生描述符。
   * @param skipped 已查询过的本域，避免再次取得同一域锁。
   * @return 固定原归属的资源租约；未注册时返回空，不跨独立 engine 查找。
   */
  resource_ptr find_resource(native_descriptor fd, const io_domain* skipped) noexcept;

 private:
  std::mutex mutex_;  ///< 串行保护所属对象的共享状态；调度与用户回调应遵循该对象的锁外调用协议。
  std::vector<std::weak_ptr<io_domain>> domains_;  ///< 供资源均衡放置使用的域弱引用表，避免延长已停止域的生命期。
  std::size_t next_{};  ///< 选择下一个 IO 域时的轮转位置。
};

}
