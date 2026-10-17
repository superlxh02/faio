/**
 * @file faio/detail/common/util/noncopyable.hpp
 * @brief 公共类型、错误与基础工具：禁止复制的基类。
 * @details 供 faio 各模块复用的头文件接口；具体所有权、错误与线程访问约束见类型及成员说明。
 */
#ifndef FAIO_DETAIL_COMMON_UTIL_NONCOPYABLE_HPP
#define FAIO_DETAIL_COMMON_UTIL_NONCOPYABLE_HPP

namespace faio::util {
class Noncopyable {
 public:
  Noncopyable(const Noncopyable&) = delete;

  Noncopyable& operator=(const Noncopyable&) = delete;

 protected:
  Noncopyable() = default;

  ~Noncopyable() noexcept = default;
};
}
#endif
