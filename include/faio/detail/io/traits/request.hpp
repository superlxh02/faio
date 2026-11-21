/**
 * @file faio/detail/io/traits/request.hpp
 * @brief 统一请求与协程注册者。
 */
#pragma once
#include <concepts>
#include <type_traits>

namespace faio::io::detail {
/** @brief 注册 I/O 请求的协程等待器。 */
template <class IO, class Request>
class IORegistrantAwaiter;

/**  @brief 只识别真正的 IORegistrantAwaiter 实例，防止自指别名误满足约束。 */
template <class T>
struct is_io_registrant_base : std::false_type {};

/** @brief 识别 I/O 请求等待器的类型特征。 */
template <class IO, class Request>
struct is_io_registrant_base<IORegistrantAwaiter<IO, Request>> : std::true_type {};

/**  @brief 完整请求、紧凑请求和兼容包装共用的精确 CRTP 操作约束，无运行时开销。 */
template <class T>
concept io_registrant_operation =
    requires { typename std::remove_cvref_t<T>::io_registrant_base_type; }
    && is_io_registrant_base<typename std::remove_cvref_t<T>::io_registrant_base_type>::value
    && std::derived_from<std::remove_cvref_t<T>,
                         typename std::remove_cvref_t<T>::io_registrant_base_type>;
}  // namespace faio::io::detail
