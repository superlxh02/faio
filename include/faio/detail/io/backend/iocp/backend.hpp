#pragma once
/**
 * @file faio/detail/io/backend/iocp/backend.hpp
 * @brief Windows 原生完成端口后端与中立 backend_protocol 的桥接。
 */
#include "faio/detail/io/protocol/backend.hpp"
#if defined(_WIN32)
#include "faio/detail/io/backend/iocp/overlapped_state.hpp"
#include "faio/detail/io/protocol/request.hpp"
#include "faio/detail/io/platform/windows/error.hpp"
#include "faio/detail/io/platform/windows/socket.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace faio::io::windows {
/**  @brief 实际 IOCP 能力；路径和目录控制接口使用独立有界文件服务。 */
struct iocp_capabilities {
  static constexpr bool native_proactor = true,
                        native_filesystem_supported = true; /**< 该原生后端是否实现文件异步操作。 */
  static constexpr bool implemented = true, overlapped_network = true, overlapped_files = true,
                        readiness = true; /**< 是否提供读写就绪观察能力。 */
};

using native_registration = ::faio::io::detail::native_registration;
using submit_status = ::faio::io::detail::backend_submit_status;
using submit_result = ::faio::io::detail::backend_submit_result;

/**
 * @brief 原生完成端口，使用地址稳定的 OVERLAPPED 池，支持批量消费和精确取消。
 * @details 内核仅保存 OVERLAPPED，不保存协程或 awaiter。请求保存在域的稳定槽中，
 * 在原完成包被消费前不得复用负载；CancelIoEx ACK 不能替代原完成包。
 * 仅在启用 FILE_SKIP_COMPLETION_PORT_ON_SUCCESS 时，同步成功才合成一次结果。
 * ready 使用不消费数据的 WSAPoll；存在观察者时，单次等待不超过 1 ms。
 * 普通网络和文件请求始终使用原生 OVERLAPPED API。
 */
class iocp_backend {
  using request_type = detail::io_request; /**< 该后端或协议使用的统一请求描述类型。 */
  using kind = detail::operation_kind;     /**< 种类标签，用于区分请求、事件或句柄。 */
  using event = detail::backend_event;     /**< 该后端对外产生的统一完成事件类型。 */
  using event_kind =
      detail::backend_event_kind; /**< 用于区分业务完成、就绪与控制回执的事件种类。 */
  using handle_kind =
      detail::native_handle_kind; /**< 区分 POSIX 描述符、Windows socket 与文件句柄的标签类型。 */
  /** @brief 后端登记项。 */
  struct entry;
  using native_prefix =
      overlapped_operation_state<entry>; /**< 标准布局前缀允许从原生 OVERLAPPED 安全恢复池节点。 */

  static_assert(std::is_standard_layout_v<native_prefix>);
  static_assert(offsetof(native_prefix, overlapped) == 0);

  /** @brief 后端登记项。 */
  struct entry {
    native_prefix native;           /**< 标准布局 OVERLAPPED 前缀，内核借用地址不变。 */
    std::uint64_t token{};          /**< 域操作的完整槽位和代际，防止迟到取消误伤复用槽。 */
    request_type* request{};        /**< 只借用已接受域请求，最终完成前域不得释放它。 */
    entry *next{}, *ready_next{};   /**< 就绪条目链表后继，与完成链表使用独立链接。 */
    std::array<WSABUF, 2> scalar{}; /**< 单次读写使用的内联缓冲区向量，避免通用堆分配。 */
    std::vector<WSABUF> vectors;    /**< 向量容量随节点复用，只复制描述符。 */
    WSAMSG message{};               /**< 稳定保存的消息描述及缓冲区引用，安全完成前不得迁移。 */
    sockaddr_storage address{};     /**< 拥有的本地或对端 socket 地址，提交期间不借用临时对象。 */
    int address_length{};           /**< 原生 socket 地址的有效字节长度。 */
    std::array<std::byte, 2 * (sizeof(sockaddr_storage) + 16)>
        accept_addresses{};          /**< AcceptEx 使用的本地与远端地址存储，保持到重叠操作完成。 */
    SOCKET accepted{INVALID_SOCKET}; /**< AcceptEx 接受的临时自有句柄，交付前失败由池清理。 */
    DWORD bytes{}, flags{};          /**< 本次原生操作的标志位，按对应系统调用语义解释。 */
    std::int64_t immediate{};        /**< 仅保存不会再产生内核包的同步结果。 */
    bool active{}, synthetic{}, skip{}, cancelled{}; /**< 本次操作是否以取消原因结束。 */
    bool short_io{}, polling{};                      /**<
                                                      * 是否处于就绪观察或轮询路径。PEEK/DONTWAIT 使用短暂的非阻塞调用，
                                                      * PEEK 的后续等待由观察链负责。
                                                      */
  };

  /** @brief 原生句柄登记记录。 */
  struct registration {
    handle_kind kind; /**< 种类标签，用于区分请求、事件或句柄。 */
    bool skip;        /**< 是否跳过本次原生动作，由外层状态仲裁决定。 */
  };

 public:
  static constexpr bool native_proactor = true,
                        poll_flushes_submissions =
                            true; /**< poll 是否负责冲刷待提交请求，避免驱动器重复提交。 */
  static constexpr const char* backend_name = "iocp"; /**< 后端名称常量，用于能力报告与诊断。 */

  /**  @brief 预热稳定池，再创建完成端口；构造阶段不接受业务请求。 */
  explicit iocp_backend(std::size_t capacity = 4096) {
    if (auto network = initialize_winsock(); !network)
      throw std::system_error(network.error().value(), std::system_category(), "WSAStartup");
    entries_.reserve(capacity);
    for (std::size_t i = 0; i < capacity; ++i) {
      auto node = std::make_unique<entry>();
      node->native.owner = node.get();
      node->next = free_;
      free_ = node.get();
      entries_.push_back(std::move(node));
    }
    port_ = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (!port_)
      throw std::system_error(::GetLastError(), std::system_category(), "CreateIoCompletionPort");
  }

  /** @brief 禁止复制构造 Windows IOCP 原生完成后端。 */
  iocp_backend(const iocp_backend&) = delete;

  /** @brief 销毁 Windows IOCP 原生完成后端。 */
  ~iocp_backend() {
    if (active_)
      std::terminate();
    if (port_)
      ::CloseHandle(port_);
  }

  /** @brief 返回实际后端用于诊断的静态名称。 */
  const char* name() const noexcept { return backend_name; }

  /**  @brief 持久关联完整位宽句柄；失败时不接管调用方的所有权。 */
  int register_resource(native_registration handle, std::uint64_t) noexcept {
    std::lock_guard lock(mutex_);
    return register_resource_locked(handle);
  }

  /**  @brief IOCP 关联不能解除；删除用户态缓存，实际关闭仍由资源负责。 */
  void unregister_resource(native_registration handle) noexcept {
    std::lock_guard lock(mutex_);
    registrations_.erase(handle.value);
  }

  /** @brief 查询初始化时固定的原生能力，避免普通请求热路径重复探测平台。 */
  bool supports(std::uint32_t value) const noexcept {
    switch (static_cast<kind>(value)) {
      case kind::recv:
      case kind::send:
      case kind::recvfrom:
      case kind::sendto:
      case kind::recvmsg:
      case kind::sendmsg:
      case kind::accept:
      case kind::connect:
      case kind::read:
      case kind::write:
      case kind::ready:
        return true;
      default:
        return false;
    }
  }

  /**  @brief 接受前取得稳定节点；同步失败不保留任何内核 payload 引用。 */
  submit_result try_submit(detail::backend_operation operation) noexcept {
    std::lock_guard lock(mutex_);
    auto& r = *operation.request;
    if (!supports(static_cast<std::uint32_t>(r.kind)) && r.kind != kind::send_zc
        && r.kind != kind::sendmsg_zc)
      return {submit_status::rejected, EOPNOTSUPP};
    if (!free_)
      return {submit_status::would_queue, 0};
    if (r.fd < 0)
      return {submit_status::rejected, EBADF};
    const native_registration handle{static_cast<std::uintptr_t>(r.fd), r.native_kind};
    if (int error = register_resource_locked(handle); error)
      return {submit_status::rejected, -error};
    auto* node = free_;
    free_ = node->next;
    node->next = nullptr;
    node->native.overlapped = {};
    node->token = operation.token;
    node->request = &r;
    node->bytes = node->flags = 0;
    node->address = {};
    node->address_length = sizeof(sockaddr_storage);
    node->message = {};
    node->accepted = INVALID_SOCKET;
    node->active = true;
    node->synthetic = node->cancelled = false;
    node->short_io = node->polling = false;
    node->skip = registrations_.at(handle.value).skip;
    r.native_completion_key = reinterpret_cast<std::uintptr_t>(node);
    try {
      if (int error = issue(*node); error) {
        recycle(*node);
        r.native_completion_key = 0;
        return {submit_status::rejected, error};
      }
    } catch (const std::invalid_argument&) {
      recycle(*node);
      r.native_completion_key = 0;
      return {submit_status::rejected, EINVAL};
    } catch (...) {
      recycle(*node);
      r.native_completion_key = 0;
      return {submit_status::rejected, ENOMEM};
    }
    ++active_;
    ++statistics_.native_submitted;
    ++statistics_.native_flushed;
    return {submit_status::accepted, 0};
  }

  /** @brief 提交缓冲的写入或已准备的后端请求，失败保留尚未完成的责任。 */
  detail::backend_flush_result flush() noexcept { return {}; }

  /**  @brief token 与请求键共同校验；取消成功后仍保留原完成包责任。 */
  void request_cancel(detail::backend_operation operation) noexcept {
    std::lock_guard lock(mutex_);
    if (!operation.request)
      return;
    auto& r = *operation.request;
    auto* node = reinterpret_cast<entry*>(static_cast<std::uintptr_t>(r.native_completion_key));
    if (!node || !node->active || node->token != operation.token || node->cancelled)
      return;
    node->cancelled = true;
    if ((r.kind == kind::ready || node->polling) && !node->synthetic) {
      remove_ready(*node);
      complete_immediate(*node, -ECANCELED);
    } else if (!node->synthetic)
      (void)::CancelIoEx(reinterpret_cast<HANDLE>(r.fd), &node->native.overlapped);
    wake();
  }

  /**  @brief 批量消费原请求的完成包；内核等待不持提交锁，允许远程 submit/cancel。 */
  int poll(std::span<event> output, std::optional<int> timeout) noexcept {
    if (output.empty())
      return 0;
    std::size_t count{};
    bool has_ready{};
    {
      std::lock_guard lock(mutex_);
      scan_ready();
      count = drain_immediate(output);
      has_ready = ready_ != nullptr;
    }
    if (count)
      return static_cast<int>(count);
    std::array<OVERLAPPED_ENTRY, 256> packets;
    ULONG removed{};
    DWORD wait = timeout ? static_cast<DWORD>(std::max(*timeout, 0)) : INFINITE;
    if (has_ready)
      wait = std::min<DWORD>(wait, 1);
    if (!::GetQueuedCompletionStatusEx(port_,
                                       packets.data(),
                                       static_cast<ULONG>(std::min(output.size(), packets.size())),
                                       &removed,
                                       wait,
                                       FALSE)) {
      DWORD error = ::GetLastError();
      if (error != WAIT_TIMEOUT)
        return -encode_windows_error(error);
    }
    {
      std::lock_guard lock(mutex_);
      for (ULONG i = 0; i < removed; ++i) {
        auto& packet = packets[i];
        if (!packet.lpOverlapped) {
          wake_pending_.store(false, std::memory_order_release);
          continue;
        }
        auto* node = reinterpret_cast<native_prefix*>(packet.lpOverlapped)->owner;
        if (!node->active || node->synthetic)
          std::terminate();
        output[count++] = {event_kind::result, node->token, decode(*node, packet), 0};
        --active_;
        ++statistics_.native_completed;
        recycle(*node);
      }
      scan_ready();
      count += drain_immediate(output.subspan(count));
    }
    return static_cast<int>(count);
  }

  /**  @brief 通知包只唤醒控制面；通过原子门合并跨线程通知。 */
  void wake() const noexcept {
    if (!wake_pending_.exchange(true, std::memory_order_acq_rel))
      if (!::PostQueuedCompletionStatus(port_, 0, 0, nullptr))
        std::terminate();
  }

  /** @brief drain 策略保留已接受的 I/O，不提前取消。 */
  void begin_shutdown() noexcept { wake(); }

  /** @brief 处理永久后端故障，仅在能够保证原生引用安全解除时转入失败排空。 */
  bool begin_failure(int) noexcept {
    std::lock_guard lock(mutex_);
    for (auto& owned : entries_) {
      auto& node = *owned;
      if (node.active && !node.synthetic && !node.polling && node.request->kind != kind::ready)
        (void)::CancelIoEx(reinterpret_cast<HANDLE>(node.request->fd), &node.native.overlapped);
    }
    wake();
    return true;
  }

  /** @brief 检查执行、内核引用和完成责任是否均已归还。 */
  bool quiescent() const noexcept {
    std::lock_guard lock(mutex_);
    return active_ == 0;
  }

  /** @brief 读取后端累计的提交、完成和控制事件统计。 */
  detail::backend_statistics statistics() const noexcept {
    std::lock_guard lock(mutex_);
    return statistics_;
  }

 private:
  /** @brief 在后端锁内把完整宽度原生句柄关联到完成端口。 */
  int register_resource_locked(native_registration handle) noexcept {
    if (handle.kind != handle_kind::windows_socket && handle.kind != handle_kind::windows_handle
        && handle.kind != handle_kind::windows_pipe)
      return -EINVAL;
    if (auto it = registrations_.find(handle.value); it != registrations_.end())
      return it->second.kind == handle.kind ? 0 : -EINVAL;
    try {
      auto [it, inserted] = registrations_.emplace(handle.value, registration{handle.kind, false});
      (void)inserted;
      HANDLE native = reinterpret_cast<HANDLE>(handle.value);
      if (!::CreateIoCompletionPort(native, port_, 0, 0)) {
        DWORD error = ::GetLastError();
        registrations_.erase(it);
        return -encode_windows_error(error);
      }
      it->second.skip =
          handle.kind != handle_kind::windows_pipe
          && ::SetFileCompletionNotificationModes(native, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)
                 != FALSE;
      return 0;
    } catch (...) {
      return -ENOMEM;
    }
  }

  /** @brief 按 socket 提供者查询 Windows 扩展函数入口，失败保留原生错误。 */
  template <class Function>
  static int extension(SOCKET socket, GUID id, Function& output) noexcept {
    DWORD bytes{};
    return ::WSAIoctl(socket,
                      SIO_GET_EXTENSION_FUNCTION_POINTER,
                      &id,
                      sizeof(id),
                      &output,
                      sizeof(output),
                      &bytes,
                      nullptr,
                      nullptr)
               ? encode_winsock_error(::WSAGetLastError())
               : 0;
  }

  /**  @brief 固定 WSABUF 数组；复制描述符而不复制业务数据，检查 ULONG 溢出。 */
  WSABUF* buffers(entry& node, DWORD& count, const iovec* vectors, std::size_t size) {
    if (size > detail::native_iov_limit || (size && !vectors))
      throw std::invalid_argument("WSABUF count");
    WSABUF* output = node.scalar.data();
    if (size > node.scalar.size()) {
      node.vectors.resize(size);
      output = node.vectors.data();
    }
    for (std::size_t i = 0; i < size; ++i) {
      if (vectors[i].iov_len > ULONG_MAX)
        throw std::invalid_argument("WSABUF length");
      output[i] = {static_cast<ULONG>(vectors[i].iov_len), static_cast<char*>(vectors[i].iov_base)};
    }
    count = static_cast<DWORD>(size);
    if (!count) {
      output[0] = {0, nullptr};
      count = 1;
    }
    return output;
  }

  /**
   *  @brief PEEK 和 DONTWAIT 使用非 OVERLAPPED Winsock，绝不在 worker 阻塞。
   * @details PEEK 的非 OVERLAPPED 形式由 Winsock 约定明确支持；DONTWAIT 必须
   *          立即交付 WSAEWOULDBLOCK，提交时须保持标志以避免长期挂起。
   *          数据报 WSAEMSGSIZE 已复制部分字节，按便携 recv 约定交付该进度。
   */
  std::int64_t short_socket(entry& node) noexcept {
    auto& r = *node.request;
    const SOCKET socket = static_cast<SOCKET>(r.fd);
    const DWORD flags = static_cast<DWORD>(r.flags & ~MSG_DONTWAIT);
    if (r.kind == kind::recvmsg) {
      const auto result = recv_message(r.fd, r.output_message, r.flags);
      return result < 0 ? -encode_winsock_error(::WSAGetLastError()) : result;
    }
    if (r.kind == kind::sendmsg || r.kind == kind::sendmsg_zc) {
      r.message.msg_iov = r.vectors.data();
      const auto result = send_message(r.fd, &r.message, r.flags);
      return result < 0 ? -encode_winsock_error(::WSAGetLastError()) : result;
    }
    node.scalar[0] = {static_cast<ULONG>(std::min<std::size_t>(r.length, ULONG_MAX)),
                      r.buffer ? static_cast<char*>(r.buffer)
                               : const_cast<char*>(static_cast<const char*>(r.const_buffer))};
    node.flags = flags;
    node.bytes = 0;
    int status{};
    switch (r.kind) {
      case kind::recv:
        status =
            ::WSARecv(socket, node.scalar.data(), 1, &node.bytes, &node.flags, nullptr, nullptr);
        break;
      case kind::recvfrom:
        node.address_length = sizeof(node.address);
        status = ::WSARecvFrom(socket,
                               node.scalar.data(),
                               1,
                               &node.bytes,
                               &node.flags,
                               reinterpret_cast<sockaddr*>(&node.address),
                               &node.address_length,
                               nullptr,
                               nullptr);
        break;
      case kind::send:
      case kind::send_zc:
        status = ::WSASend(socket, node.scalar.data(), 1, &node.bytes, flags, nullptr, nullptr);
        break;
      case kind::sendto:
        status = ::WSASendTo(socket,
                             node.scalar.data(),
                             1,
                             &node.bytes,
                             flags,
                             reinterpret_cast<const sockaddr*>(&r.address),
                             r.address_length,
                             nullptr,
                             nullptr);
        break;
      default:
        return -EOPNOTSUPP;
    }
    if (status == SOCKET_ERROR) {
      const int error = ::WSAGetLastError();
      if (error != WSAEMSGSIZE || (r.kind != kind::recv && r.kind != kind::recvfrom))
        return -encode_winsock_error(error);
    }
    return node.bytes;
  }

  /**
   *  @brief 每种业务请求只调用对应的原生 API，PENDING
   * 与同步成功各保留完成责任。
   */
  int issue(entry& node) {
    auto& r = *node.request;
    auto* overlapped = &node.native.overlapped;
    SOCKET socket = static_cast<SOCKET>(r.fd);
    int status = SOCKET_ERROR;
    if (r.kind == kind::ready) {
      node.ready_next = ready_;
      ready_ = &node;
      return 0;
    }
    const bool receive =
        r.kind == kind::recv || r.kind == kind::recvfrom || r.kind == kind::recvmsg;
    const bool transmit = r.kind == kind::send || r.kind == kind::send_zc || r.kind == kind::sendto
                          || r.kind == kind::sendmsg || r.kind == kind::sendmsg_zc;
    if (r.kind == kind::recv && !r.length) {
      int type{}, size = sizeof(type);
      if (::getsockopt(socket, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &size))
        return encode_winsock_error(::WSAGetLastError());
      if (type == SOCK_STREAM) {
        complete_immediate(node, 0);
        return 0;
      }
    }
    if ((receive || transmit) && ((r.flags & MSG_DONTWAIT) || (receive && (r.flags & MSG_PEEK)))) {
      node.short_io = true;
      const auto result = short_socket(node);
      if (result == -encode_winsock_error(WSAEWOULDBLOCK) && !(r.flags & MSG_DONTWAIT)) {
        node.polling = true;

        node.ready_next = ready_;
        ready_ = &node;
        return 0;
      }
      if (result < 0)
        return static_cast<int>(-result);
      complete_immediate(node, result);
      return 0;
    }
    if (r.kind == kind::read || r.kind == kind::write) {
      if (r.native_kind != handle_kind::windows_handle
          && r.native_kind != handle_kind::windows_pipe)
        return EINVAL;
      const auto offset = r.native_kind == handle_kind::windows_pipe || r.windows_implicit_cursor
                              ? std::uint64_t{0}
                              : r.offset;
      overlapped->Offset = static_cast<DWORD>(offset);
      overlapped->OffsetHigh = static_cast<DWORD>(offset >> 32);
      DWORD length = static_cast<DWORD>(std::min<std::size_t>(r.length, MAXDWORD));
      BOOL success =
          r.kind == kind::read
              ? ::ReadFile(
                    reinterpret_cast<HANDLE>(r.fd), r.buffer, length, &node.bytes, overlapped)
              : ::WriteFile(reinterpret_cast<HANDLE>(r.fd),
                            r.const_buffer,
                            length,
                            &node.bytes,
                            overlapped);
      if (success) {
        if (node.skip)
          complete_immediate(node, node.bytes);
        return 0;
      }
      DWORD error = ::GetLastError();
      if (error == ERROR_IO_PENDING)
        return 0;
      if (r.kind == kind::read
          && (error == ERROR_HANDLE_EOF
              || (r.native_kind == handle_kind::windows_pipe
                  && (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED)))) {
        complete_immediate(node, 0);
        return 0;
      }
      return encode_windows_error(error);
    }
    if (r.native_kind != handle_kind::windows_socket)
      return EINVAL;
    if (r.kind == kind::connect) {
      int type{}, length = sizeof(type);
      if (::getsockopt(socket, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &length))
        return encode_winsock_error(::WSAGetLastError());
      if (type == SOCK_DGRAM) {
        if (::connect(socket, reinterpret_cast<sockaddr*>(&r.address), r.address_length))
          return encode_winsock_error(::WSAGetLastError());
        complete_immediate(node, 0);
        return 0;
      }
      LPFN_CONNECTEX function{};
      GUID id = WSAID_CONNECTEX;
      if (int error = extension(socket, id, function); error)
        return error;
      sockaddr_storage local{};
      int size = sizeof(local);
      if (::getsockname(socket, reinterpret_cast<sockaddr*>(&local), &size) == SOCKET_ERROR) {
        local.ss_family = r.address.ss_family;
        size = local.ss_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
        if (::bind(socket, reinterpret_cast<sockaddr*>(&local), size))
          return encode_winsock_error(::WSAGetLastError());
      }
      BOOL success = function(socket,
                              reinterpret_cast<sockaddr*>(&r.address),
                              r.address_length,
                              nullptr,
                              0,
                              &node.bytes,
                              overlapped);
      if (success) {
        if (node.skip)
          complete_immediate(node, 0);
        return 0;
      }
      int error = ::WSAGetLastError();
      return error == WSA_IO_PENDING ? 0 : encode_winsock_error(error);
    }
    if (r.kind == kind::accept) {
      LPFN_ACCEPTEX function{};
      GUID id = WSAID_ACCEPTEX;
      if (int error = extension(socket, id, function); error)
        return error;
      sockaddr_storage local{};
      int size = sizeof(local);
      if (::getsockname(socket, reinterpret_cast<sockaddr*>(&local), &size))
        return encode_winsock_error(::WSAGetLastError());
      node.accepted = ::WSASocketW(local.ss_family,
                                   SOCK_STREAM,
                                   IPPROTO_TCP,
                                   nullptr,
                                   0,
                                   WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
      if (node.accepted == INVALID_SOCKET)
        return encode_winsock_error(::WSAGetLastError());
      BOOL success = function(socket,
                              node.accepted,
                              node.accept_addresses.data(),
                              0,
                              sizeof(sockaddr_storage) + 16,
                              sizeof(sockaddr_storage) + 16,
                              &node.bytes,
                              overlapped);
      if (success) {
        if (node.skip)
          complete_immediate(node, 0);
        return 0;
      }
      int error = ::WSAGetLastError();
      return error == WSA_IO_PENDING ? 0 : encode_winsock_error(error);
    }
    if (r.kind == kind::recvmsg || r.kind == kind::sendmsg || r.kind == kind::sendmsg_zc) {
      auto& message = r.kind == kind::recvmsg ? *r.output_message : r.message;
      auto* vectors = r.kind == kind::recvmsg ? message.msg_iov : r.vectors.data();
      auto size = r.kind == kind::recvmsg ? message.msg_iovlen : r.vectors.size();
      node.message.lpBuffers = buffers(node, node.message.dwBufferCount, vectors, size);
      node.message.name = static_cast<sockaddr*>(message.msg_name);
      node.message.namelen = message.msg_namelen;
      if (message.msg_controllen > ULONG_MAX)
        return EINVAL;
      node.message.Control = {static_cast<ULONG>(message.msg_controllen),
                              static_cast<char*>(message.msg_control)};
      node.message.dwFlags = static_cast<DWORD>(r.flags & ~MSG_DONTWAIT);
      if (r.kind == kind::recvmsg && !message.msg_name && !message.msg_control) {
        node.flags = node.message.dwFlags;
        status = ::WSARecv(socket,
                           node.message.lpBuffers,
                           node.message.dwBufferCount,
                           &node.bytes,
                           &node.flags,
                           overlapped,
                           nullptr);
      } else if (r.kind != kind::recvmsg && !message.msg_control) {
        status = message.msg_name ? ::WSASendTo(socket,
                                                node.message.lpBuffers,
                                                node.message.dwBufferCount,
                                                &node.bytes,
                                                node.message.dwFlags,
                                                static_cast<sockaddr*>(message.msg_name),
                                                message.msg_namelen,
                                                overlapped,
                                                nullptr)
                                  : ::WSASend(socket,
                                              node.message.lpBuffers,
                                              node.message.dwBufferCount,
                                              &node.bytes,
                                              node.message.dwFlags,
                                              overlapped,
                                              nullptr);
      } else if (r.kind == kind::recvmsg) {
        LPFN_WSARECVMSG function{};
        GUID id = WSAID_WSARECVMSG;
        if (int error = extension(socket, id, function); error)
          return error;
        status = function(socket, &node.message, &node.bytes, overlapped, nullptr);
      } else {
        LPFN_WSASENDMSG function{};
        GUID id = WSAID_WSASENDMSG;
        if (int error = extension(socket, id, function); error)
          return error;
        status =
            function(socket, &node.message, node.message.dwFlags, &node.bytes, overlapped, nullptr);
      }
    } else {
      node.scalar[0] = {static_cast<ULONG>(std::min<std::size_t>(r.length, ULONG_MAX)),
                        r.buffer ? static_cast<char*>(r.buffer)
                                 : const_cast<char*>(static_cast<const char*>(r.const_buffer))};
      node.flags = static_cast<DWORD>(r.flags & ~MSG_DONTWAIT);
      switch (r.kind) {
        case kind::recv:
          status = ::WSARecv(
              socket, node.scalar.data(), 1, &node.bytes, &node.flags, overlapped, nullptr);
          break;
        case kind::send:
        case kind::send_zc:
          status = ::WSASend(
              socket, node.scalar.data(), 1, &node.bytes, node.flags, overlapped, nullptr);
          break;
        case kind::recvfrom:
          status = ::WSARecvFrom(socket,
                                 node.scalar.data(),
                                 1,
                                 &node.bytes,
                                 &node.flags,
                                 reinterpret_cast<sockaddr*>(&node.address),
                                 &node.address_length,
                                 overlapped,
                                 nullptr);
          break;
        case kind::sendto:
          status = ::WSASendTo(socket,
                               node.scalar.data(),
                               1,
                               &node.bytes,
                               node.flags,
                               reinterpret_cast<sockaddr*>(&r.address),
                               r.address_length,
                               overlapped,
                               nullptr);
          break;
        default:
          return EOPNOTSUPP;
      }
    }
    if (status == 0) {
      if (node.skip)
        complete_immediate(node, node.bytes);
      return 0;
    }
    int error = ::WSAGetLastError();
    if (error == WSAEMSGSIZE && receive) {
      if (r.kind == kind::recvmsg) {
        node.flags |= MSG_TRUNC;
        if (!(node.message.dwFlags & (MSG_TRUNC | MSG_CTRUNC)))
          node.message.dwFlags |= MSG_TRUNC;
      }
      complete_immediate(node, node.bytes);
      return 0;
    }
    return error == WSA_IO_PENDING ? 0 : encode_winsock_error(error);
  }

  /**
   * @brief 完成后更新 socket 上下文及输出；若取消与接受成功竞争，由域清理新接受的 socket。
   */
  std::int64_t finalize(entry& node, std::int64_t result) noexcept {
    auto& r = *node.request;
    if (result < 0)
      return result;
    if (node.short_io
        && (r.kind == kind::recvmsg || r.kind == kind::sendmsg || r.kind == kind::sendmsg_zc))
      return result;
    SOCKET socket = static_cast<SOCKET>(r.fd);
    if (r.kind == kind::accept) {
      if (::setsockopt(node.accepted,
                       SOL_SOCKET,
                       SO_UPDATE_ACCEPT_CONTEXT,
                       reinterpret_cast<const char*>(&socket),
                       sizeof(socket)))
        return -encode_winsock_error(::WSAGetLastError());
      u_long enabled = 1;
      if (::ioctlsocket(node.accepted, FIONBIO, &enabled))
        return -encode_winsock_error(::WSAGetLastError());
      if (r.output_address && r.output_address_length) {
        int size = *r.output_address_length;
        if (::getpeername(node.accepted, r.output_address, &size))
          return -encode_winsock_error(::WSAGetLastError());
        *r.output_address_length = size;
      }
      return static_cast<std::int64_t>(std::exchange(node.accepted, INVALID_SOCKET));
    }
    if (r.kind == kind::connect) {
      int type{}, length = sizeof(type);
      if (::getsockopt(socket, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &length))
        return -encode_winsock_error(::WSAGetLastError());
      if (type == SOCK_STREAM
          && ::setsockopt(socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0))
        return -encode_winsock_error(::WSAGetLastError());
      return 0;
    }
    if (r.kind == kind::recvfrom && r.output_address && r.output_address_length) {
      int size = std::min<int>(*r.output_address_length, node.address_length);
      if (size > 0)
        std::memcpy(r.output_address, &node.address, static_cast<std::size_t>(size));
      *r.output_address_length = node.address_length;
    }
    if (r.kind == kind::recvmsg && r.output_message) {
      if (!r.output_message->msg_name && !r.output_message->msg_control)
        node.message.dwFlags = node.flags;
      r.output_message->msg_flags = static_cast<int>(node.message.dwFlags);
      r.output_message->msg_namelen = node.message.namelen;
      r.output_message->msg_controllen = node.message.Control.len;
    }
    return result;
  }

  /**
   * @brief 将批量 API 完成包中的 NTSTATUS，通过对应原生 API 转换为公开错误域。
   * @details GetQueuedCompletionStatusEx 成功仅表示取到完成包，不代表业务成功。
   * 此处传入 FALSE 仅读取已完成的状态，不会在驱动线程中再次等待。
   */
  std::int64_t decode(entry& node, const OVERLAPPED_ENTRY& packet) noexcept {
    std::int64_t result = packet.dwNumberOfBytesTransferred;
    if (packet.Internal != 0) {
      DWORD bytes{}, flags{};
      auto& r = *node.request;
      if (r.native_kind == handle_kind::windows_socket) {
        if (!::WSAGetOverlappedResult(
                static_cast<SOCKET>(r.fd), &node.native.overlapped, &bytes, FALSE, &flags)) {
          int error = ::WSAGetLastError();
          if (error == WSAEMSGSIZE
              && (r.kind == kind::recv || r.kind == kind::recvfrom || r.kind == kind::recvmsg)) {
            if (r.kind == kind::recvmsg) {
              node.flags |= MSG_TRUNC;
              if (!(node.message.dwFlags & (MSG_TRUNC | MSG_CTRUNC)))
                node.message.dwFlags |= MSG_TRUNC;
            }
            result = bytes;
          } else
            result = -encode_winsock_error(error);
        } else
          result = bytes;
      } else if (!::GetOverlappedResult(
                     reinterpret_cast<HANDLE>(r.fd), &node.native.overlapped, &bytes, FALSE)) {
        DWORD error = ::GetLastError();
        result =
            r.kind == kind::read
                    && (error == ERROR_HANDLE_EOF
                        || (r.native_kind == handle_kind::windows_pipe
                            && (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED)))
                ? 0
                : -encode_windows_error(error);
      } else
        result = bytes;
    }
    return finalize(node, result);
  }

  /**  @brief 记录无后续内核包的结果；不调用域回调，不提前复用节点。 */
  void complete_immediate(entry& node, std::int64_t result) noexcept {
    node.synthetic = true;
    node.immediate = result;
    node.next = nullptr;
    if (done_tail_)
      done_tail_->next = &node;
    else
      done_ = &node;
    done_tail_ = &node;
  }

  /**  @brief 合成完成和内核完成具有相同的计数、finalize 与回收边界。 */
  std::size_t drain_immediate(std::span<event> output) noexcept {
    std::size_t count{};
    while (done_ && count < output.size()) {
      auto* node = done_;
      done_ = node->next;
      if (!done_)
        done_tail_ = nullptr;
      output[count++] = {event_kind::result, node->token, finalize(*node, node->immediate), 0};
      --active_;
      ++statistics_.native_completed;
      recycle(*node);
    }
    return count;
  }

  /** @brief 从内部就绪链移除条目，避免重复交付同一观察结果。 */
  void remove_ready(entry& node) noexcept {
    auto** link = &ready_;
    while (*link && *link != &node)
      link = &(*link)->ready_next;
    if (*link)
      *link = node.ready_next;
    node.ready_next = nullptr;
  }

  /**  @brief 每个观察者独立查询，不合并消费、不执行零字节 recv 或 peek。 */
  void scan_ready() noexcept {
    for (auto* node = ready_; node;) {
      auto* next = node->ready_next;
      auto& r = *node->request;
      WSAPOLLFD descriptor{
          static_cast<SOCKET>(r.fd),
          static_cast<SHORT>(node->polling ? POLLRDNORM
                                           : ((r.argument & 1 ? POLLRDNORM : 0)
                                              | (r.argument & 2 ? POLLWRNORM : 0))),
          0};
      int result = ::WSAPoll(&descriptor, 1, 0);
      if (result < 0 || descriptor.revents) {
        if (node->polling && result >= 0) {
          const auto observed = short_socket(*node);
          if (observed == -encode_winsock_error(WSAEWOULDBLOCK)) {
            node = next;
            continue;
          }
          remove_ready(*node);
          complete_immediate(*node, observed);
          node = next;
          continue;
        }
        remove_ready(*node);
        complete_immediate(
            *node, result < 0 ? -encode_winsock_error(::WSAGetLastError()) : descriptor.revents);
      }
      node = next;
    }
  }

  /**  @brief 只在拒绝前或最终结果消费后归还池；接受失败的临时 socket 不泄漏。 */
  void recycle(entry& node) noexcept {
    if (node.accepted != INVALID_SOCKET) {
      ::closesocket(node.accepted);
      node.accepted = INVALID_SOCKET;
    }
    node.active = false;
    node.request = nullptr;
    node.ready_next = nullptr;
    node.next = free_;
    free_ = &node;
  }

  HANDLE port_{};            /**< 拥有的 IOCP 完成端口句柄，后端销毁时释放。 */
  mutable std::mutex mutex_; /**< 保护后端的共享状态；调度与用户回调应遵循锁外调用协议。 */
  mutable std::atomic<bool>
      wake_pending_{}; /**< 尚未被轮询方消费的控制通知，合并重复唤醒但保留通知责任。 */
  std::vector<std::unique_ptr<entry>>
      entries_; /**< 拥有地址稳定的 IOCP 操作条目，回收前内核不得再引用。 */
  std::unordered_map<std::uintptr_t, registration>
      registrations_;                                 /**< 原生句柄到资源键的长期注册表。 */
  entry *free_{}, *done_{}, *done_tail_{}, *ready_{}; /**< 待交付的就绪结果或就绪条目链表头。 */
  std::size_t active_{};                              /**< 尚未消费最终完成的已接受后端请求数量。 */
  detail::backend_statistics statistics_{};           /**< 后端提交、完成及控制事件的诊断统计。 */
};

}  // namespace faio::io::windows
#endif
