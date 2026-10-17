/**
 * @file faio/detail/io/traits/request.hpp
 * @brief 跨平台异步 IO：统一请求与协程注册者。
 * @details 以编译期约束检查接口形状及返回类型；概念检查本身不执行 IO，也不承担资源生命周期。
 */
#pragma once
#include <concepts>
#include <type_traits>

namespace faio::io::detail {
template <class IO, class Request>
class IORegistrantAwaiter;

/** @brief 只识别真正的 IORegistrantAwaiter 实例，防止自指别名误满足约束。 */
template <class T>
struct is_io_registrant_base : std::false_type {};

template <class IO, class Request>
struct is_io_registrant_base<IORegistrantAwaiter<IO, Request>> : std::true_type {};

/** @brief 完整/紧凑及兼容包装共享的精确 CRTP 操作约束，无运行时成本。 */
template <class T>
concept io_registrant_operation =
    requires { typename std::remove_cvref_t<T>::io_registrant_base_type; }
    && is_io_registrant_base<typename std::remove_cvref_t<T>::io_registrant_base_type>::value
    && std::derived_from<std::remove_cvref_t<T>,
                         typename std::remove_cvref_t<T>::io_registrant_base_type>;
}
