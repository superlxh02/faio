/**
 * @file faio/detail/io/core/request_rules.hpp
 * @brief 请求方向与快速完成规则。
 */
#pragma once
#include "faio/detail/io/protocol/request.hpp"
#include <algorithm>

namespace faio::io::detail {
/** @brief 按操作种类计算占用读方向、写方向或仅观察的执行权。 */
inline std::uint32_t direction_of(operation_kind kind) noexcept {
  switch (kind) {
    case operation_kind::recv:
    case operation_kind::recvfrom:
    case operation_kind::recvmsg:
    case operation_kind::accept:
    case operation_kind::accept_nowait:
    case operation_kind::read:
    case operation_kind::readv:
      return 1;
    case operation_kind::send:
    case operation_kind::sendto:
    case operation_kind::sendmsg:
    case operation_kind::send_zc:
    case operation_kind::sendmsg_zc:
    case operation_kind::connect:
    case operation_kind::write:
    case operation_kind::writev:
      return 2;
    default:
      return 0;
  }
}

/** @brief 检查请求是否显式允许按字节流空读写规则立即完成。 */
inline bool empty_stream_request(const io_request& request) noexcept {
  if (!request.empty_success)
    return false;
  switch (request.kind) {
    case operation_kind::recv:
    case operation_kind::send:
    case operation_kind::send_zc:
    case operation_kind::read:
    case operation_kind::write:
      return request.length == 0;
    case operation_kind::readv:
    case operation_kind::writev:
    case operation_kind::sendmsg:
    case operation_kind::sendmsg_zc:
      return std::all_of(request.vectors.begin(), request.vectors.end(), [](const iovec& vector) {
        return vector.iov_len == 0;
      });
    case operation_kind::recvmsg:
      if (!request.output_message)
        return false;
      if (static_cast<std::uint64_t>(request.output_message->msg_iovlen) > native_iov_limit)
        return false;
      for (std::size_t i = 0; i < static_cast<std::size_t>(request.output_message->msg_iovlen); ++i)
        if (!request.output_message->msg_iov || request.output_message->msg_iov[i].iov_len != 0)
          return false;
      return true;
    default:
      return false;
  }
}
}  // namespace faio::io::detail
