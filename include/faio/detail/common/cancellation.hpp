/**
 * @file faio/detail/common/cancellation.hpp
 * @brief 公共类型、错误与基础工具：取消异常策略。
 * @details 供 faio 各模块复用的头文件接口；具体所有权、错误与线程访问约束见类型及成员说明。
 */
#ifndef FAIO_DETAIL_COMMON_CANCELLATION_HPP
#define FAIO_DETAIL_COMMON_CANCELLATION_HPP
#include <exception>

namespace faio {
/// @brief 等待操作接受 stop 请求后抛出；协程代码可显式捕获并完成自己的清理。
struct operation_cancelled : std::exception {
  const char* what() const noexcept override { return "协程等待操作已取消"; }
};
}
#endif
