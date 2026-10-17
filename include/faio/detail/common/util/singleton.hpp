/**
 * @file faio/detail/common/util/singleton.hpp
 * @brief 公共类型、错误与基础工具：单实例工具。
 * @details 供 faio 各模块复用的头文件接口；具体所有权、错误与线程访问约束见类型及成员说明。
 */
#ifndef FAIO_DETAIL_COMMON_UTIL_SINGLETON_HPP
#define FAIO_DETAIL_COMMON_UTIL_SINGLETON_HPP

namespace faio::util {
template <typename T>
class Singleton {
  Singleton() = delete;

  ~Singleton() = delete;

 public:
  [[nodiscard]]
  static auto instance() -> T& {
    static T instance;
    return instance;
  }
};
}
#endif
