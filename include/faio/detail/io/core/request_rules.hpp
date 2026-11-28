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
    case operation_kind::RECV:
    case operation_kind::RECVFROM:
    case operation_kind::RECVMSG:
    case operation_kind::ACCEPT:
    case operation_kind::ACCEPT_NOWAIT:
    case operation_kind::READ:
    case operation_kind::READV:
      return 1;
    case operation_kind::SEND:
    case operation_kind::SENDTO:
    case operation_kind::SENDMSG:
    case operation_kind::SEND_ZC:
    case operation_kind::SENDMSG_ZC:
    case operation_kind::CONNECT:
    case operation_kind::WRITE:
    case operation_kind::WRITEV:
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
    case operation_kind::RECV:
    case operation_kind::SEND:
    case operation_kind::SEND_ZC:
    case operation_kind::READ:
    case operation_kind::WRITE:
      return request.length == 0;
    case operation_kind::READV:
    case operation_kind::WRITEV:
    case operation_kind::SENDMSG:
    case operation_kind::SENDMSG_ZC:
      return std::all_of(request.vectors.begin(), request.vectors.end(), [](const iovec& vector) {
        return vector.iov_len == 0;
      });
    case operation_kind::RECVMSG:
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
