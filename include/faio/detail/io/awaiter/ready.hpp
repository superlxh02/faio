/**
 * @file faio/detail/io/awaiter/ready.hpp
 * @brief 跨平台异步 IO：就绪观察。
 * @details 构造阶段只保存参数，真正准备与提交发生在挂起阶段。借用负载与输出存储须保持到安全完成，立即完成和跨线程完成通过统一握手避免重复恢复。
 */
#pragma once
#include "faio/detail/io/awaiter/request.hpp"

namespace faio::io::detail {
/** @brief 多观察者 ready awaiter 不消费数据；关闭/取消与读写遵循同一终态协议。
 */
class ReadyAwaiter : public IORegistrantAwaiter<ReadyAwaiter> {
 public:
  ReadyAwaiter(resource_ptr resource, Interest interest)
      : IORegistrantAwaiter{[&] {
          auto r = make_request(resource, operation_kind::ready);
          /// @brief 保存操作专用的首个控制参数，真正执行时按当前操作种类解释。
          r.argument = static_cast<int>(interest);
          /// @brief 按值交出完整初始化的请求参数；真正取得稳定操作槽与提交 IO 仍发生在 await_suspend。
          return r;
        }()} {}

  ReadyAwaiter(native_descriptor fd, Interest interest)
      : IORegistrantAwaiter{[&] {
          /// @brief 构造具有确定默认状态的请求描述，随后只设置当前操作真正需要的参数。
          io_request r;
          /// @brief 保存完整宽度的原生句柄；构造阶段不登记句柄或发起系统调用。
          r.fd = fd;
          /// @brief 固定本次操作种类，域与后端据此选择对应的方向执行权及原生提交路径。
          r.kind = operation_kind::ready;
          /// @brief 保存操作专用的首个控制参数，真正执行时按当前操作种类解释。
          r.argument = static_cast<int>(interest);
          /// @brief 按值交出完整初始化的请求参数；真正取得稳定操作槽与提交 IO 仍发生在 await_suspend。
          return r;
        }()} {}

  /// @brief 在完成交接后领取结果，按该接口约定传播失败或取消。
  expected<Ready> await_resume() const noexcept {
    if (_user_data.result < 0)
      return std::unexpected{decode_io_error(static_cast<int>(-_user_data.result))};
    return Ready{static_cast<std::uint32_t>(_user_data.result), _user_data.transferred};
  }
};

}
