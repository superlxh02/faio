/**
 * @file faio/detail/io/core/placement.hpp
 * @brief 资源所属域选择。
 */
#pragma once
#include "faio/detail/io/core/fwd.hpp"
#include "faio/detail/io/platform/native_types.hpp"
#include <cstddef>
#include <mutex>
#include <vector>

namespace faio::io::detail {
/**
 *  @brief 运行时控制面共享的分片弱引用表，用于首次分配资源和兼容裸 fd 的查找。
 * @details weak_ptr 避免 domain 与 runtime 形成强引用环；选择时在短锁内轮转。
 *          已注册 I/O 的常规执行路径不访问此表，也不迁移资源。
 */
class io_placement_group {
 public:
  /** @brief 构造 I/O 资源放置组。 */
  explicit io_placement_group(std::size_t count) : domains_(count) {}

  /** @brief 把 IO 域加入放置服务的弱引用表，不延长已退出域的生命周期。 */
  void register_domain(std::size_t index, const std::shared_ptr<io_domain>& domain) noexcept {
    std::lock_guard lock(mutex_);
    domains_[index] = domain;
  }

  /** @brief 按轮转顺序选择可接纳资源的 I/O 域。 */
  std::shared_ptr<io_domain> select() noexcept;

  /**
   *  @brief 在同一 runtime 内查找已有 fd 身份，不建立新的资源或原生请求。
   * @param fd 调用方仍持有的原生描述符。
   * @param skipped 已查询过的本域，避免再次取得同一域锁。
   * @return 固定原归属的资源租约；未注册时返回空，不跨独立 engine 查找。
   */
  resource_ptr find_resource(native_descriptor fd, const io_domain* skipped) noexcept;

 private:
  std::mutex mutex_; /**< 保护域注册表及轮转位置。 */
  std::vector<std::weak_ptr<io_domain>>
      domains_;        /**< 供资源均衡放置使用的域弱引用表，避免延长已停止域的生命周期。 */
  std::size_t next_{}; /**< 选择下一个 IO 域时的轮转位置。 */
};

}  // namespace faio::io::detail
