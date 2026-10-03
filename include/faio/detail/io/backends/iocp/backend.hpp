#pragma once
/** @file backend.hpp
 * @brief Windows 原生完成端口后端与中立 backend_protocol 的桥接。
 * @details 本文件只负责内核请求与完成包；协程恢复、截止时间和资源方向门
 *          由 io_domain 负责，后端不会保存协程句柄或恢复业务代码。
 */
#include "faio/detail/io/backend_protocol.hpp"
#if defined(_WIN32)
#include "faio/detail/io/backends/iocp/overlapped_state.hpp"
#include "faio/detail/io/operation.hpp"
#include "faio/detail/io/platform/windows_error.hpp"
#include "faio/detail/io/platform/windows_socket.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace faio::io::windows {
/** @brief 实际 IOCP 能力；路径和目录控制接口使用独立有界文件服务。 */
struct iocp_capabilities {
  static constexpr bool native_proactor = true, native_filesystem_supported = true;
  static constexpr bool implemented = true, overlapped_network = true, overlapped_files = true,
                        readiness = true;
};

using backend_box = ::faio::io::detail::backend_box;
using native_registration = ::faio::io::detail::native_registration;
using submit_status = ::faio::io::detail::backend_submit_status;
using submit_result = ::faio::io::detail::backend_submit_result;

/**
 * @brief 原生完成端口：稳定池化 OVERLAPPED、批量消费以及精确取消。
 * @details 内核只保存 OVERLAPPED，不保存协程或 awaiter。request 位于域稳定槽，
 *          原完成包消费以前不可复用 payload。CancelIoEx ACK 不能替代原完成包。
 *          同步成功只有启用 FILE_SKIP_COMPLETION_PORT_ON_SUCCESS
 * 时合成一次结果。 ready 使用非消费 WSAPoll，只有存在观察者时将等待切为至多
 * 1ms； 普通网络和文件请求始终使用真正的原生 OVERLAPPED API。
 */
class iocp_backend {
  using request_type = detail::io_request;
  using kind = detail::operation_kind;
  using event = detail::backend_event;
  using event_kind = detail::backend_event_kind;
  using handle_kind = detail::native_handle_kind;
  struct entry;
  /** @brief 标准布局前缀允许从原生 OVERLAPPED 安全恢复池节点。 */
  using native_prefix = overlapped_operation_state;

  struct entry {
    native_prefix native;            ///< 标准布局 OVERLAPPED 前缀，内核借用地址不变。
    std::uint64_t token{};           ///< 域操作的完整槽位和代际，防止迟到取消误伤复用槽。
    request_type* request{};         ///< 只借用已接受域请求，最终完成前域不得释放它。
    entry *next{}, *ready_next{};    ///< 独立的完成/空闲链与观察者链。
    std::array<WSABUF, 2> scalar{};  ///< 单次读写没有通用堆分配。
    std::vector<WSABUF> vectors;     ///< 向量容量随节点复用，只复制描述符。
    WSAMSG message{};
    sockaddr_storage address{};
    int address_length{};
    std::array<std::byte, 2 * (sizeof(sockaddr_storage) + 16)> accept_addresses{};
    SOCKET accepted{INVALID_SOCKET};  ///< AcceptEx 接受的临时拥有句柄，交付前失败由池清理。
    DWORD bytes{}, flags{};           ///< Winsock 要求异步调用参数保留到完成。
    std::int64_t immediate{};         ///< 仅保存不会再产生内核包的同步结果。
    bool active{}, synthetic{}, skip{}, cancelled{};  ///< 接受、合成、跳包和取消状态互不替代。
    bool short_io{}, polling{};
    ///< PEEK/DONTWAIT 用短非阻塞调用；PEEK
    ///< 的等待责任由观察链保留。
  };

  struct registration {
    handle_kind kind;
    bool skip;
  };

 public:
  static constexpr bool native_proactor = true, poll_flushes_submissions = true;
  static constexpr const char* backend_name = "iocp";

  /** @brief 预热稳定池，再创建完成端口；构造阶段不接受业务请求。 */
  explicit iocp_backend(std::size_t capacity = 4096) {
    if (auto network = initialize_winsock(); !network)
      throw std::system_error(network.error().value(), std::system_category(), "WSAStartup");
    entries_.reserve(capacity);  // 所有者容器的地址不被内核借用。
    for (std::size_t i = 0; i < capacity; ++i) {
      auto node = std::make_unique<entry>();
      node->native.owner = node.get();      // 内核前缀和真实节点身份固定到最终包。
      node->next = free_;                   // 使用节点内链表，提交热路径不分配队列节点。
      free_ = node.get();                   // 只有未被内核借用的节点进入空闲链。
      entries_.push_back(std::move(node));  // unique_ptr 转移不移动节点本身。
    }
    port_ = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0,
                                     1);  // 每域一个完成消费者。
    if (!port_)
      throw std::system_error(::GetLastError(), std::system_category(), "CreateIoCompletionPort");
  }

  iocp_backend(const iocp_backend&) = delete;

  ~iocp_backend() {
    if (active_)
      std::terminate();  // 宿主必须先排空，绝不能释放内核仍借用的状态。
    if (port_)
      ::CloseHandle(port_);
  }

  const char* name() const noexcept { return backend_name; }

  /** @brief 持久关联完整位宽句柄；失败时不接管调用方的所有权。 */
  int attach(native_registration handle, std::uint64_t) noexcept {
    std::lock_guard lock(mutex_);
    return attach_locked(handle);
  }

  /** @brief IOCP 关联不能解除；删除用户态缓存，实际关闭仍由资源负责。 */
  void detach(native_registration handle) noexcept {
    std::lock_guard lock(mutex_);
    registrations_.erase(handle.value);
  }

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
        return false;  // 无异步原生接口的控制操作由 domain 的服务 lane 执行。
    }
  }

  /** @brief 接受前取得稳定节点；同步失败不保留任何内核 payload 引用。 */
  submit_result try_submit(detail::backend_operation operation) noexcept {
    std::lock_guard lock(mutex_);                              // 节点取得、提交与取消共用短锁。
    auto& r = *static_cast<request_type*>(operation.request);  // 统一函数表只传稳定请求地址。
    if (!supports(static_cast<std::uint32_t>(r.kind)) && r.kind != kind::send_zc
        && r.kind != kind::sendmsg_zc)
      return {submit_status::rejected, EOPNOTSUPP};
    if (!free_)  // 有界背压在接受前返回；域仍拥有尚未提交的请求。
      return {submit_status::would_queue, 0};
    if (r.fd < 0)  // HANDLE/SOCKET 保存为完整位宽，-1 是统一的无效标记。
      return {submit_status::rejected, EBADF};
    const native_registration handle{static_cast<std::uintptr_t>(r.fd), r.native_kind};
    if (int error = attach_locked(handle); error)
      return {submit_status::rejected, -error};
    auto* node = free_;             // 此节点直到原生结果消费后才允许再次进入空闲链。
    free_ = node->next;             // 取得稳定节点，避免内核借用可扩容 vector 元素。
    node->next = nullptr;           // 清除前一代的空闲链链接。
    node->native.overlapped = {};   // 原生事件和偏移不会继承前一代状态。
    node->token = operation.token;  // 取消时必须同时匹配请求键和完整代际。
    node->request = &r;             // 参数、地址和借用缓冲区都由域的稳定槽保活。
    node->bytes = node->flags = 0;
    node->address = {};
    node->address_length = sizeof(sockaddr_storage);
    node->message = {};
    node->accepted = INVALID_SOCKET;
    node->active = true;
    node->synthetic = node->cancelled = false;
    node->short_io = node->polling = false;             // 每代重置，不继承上一个观察请求的模式。
    node->skip = registrations_.at(handle.value).skip;  // 每句柄保存实际跳包设置结果。
    r.native_completion_key =
        reinterpret_cast<std::uintptr_t>(node);  // 取消 O(1) 找到原 OVERLAPPED。
    try {
      if (int error = issue(*node); error) {
        recycle(*node);               // 同步失败没有内核借用；临时 accept 句柄一并清理。
        r.native_completion_key = 0;  // 被拒绝的请求不能留下可供取消的旧地址。
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
    ++active_;                            // 已接受的原生/合成结果都承担一次最终完成责任。
    ++statistics_.native_submitted;       // 统计接受数，不统计 rejected/would_queue。
    ++statistics_.native_flushed;         // IOCP 原 API 即提交，无 Linux 式延迟 enter。
    return {submit_status::accepted, 0};  // IOCP 在原 API 中提交，无额外 flush 队列。
  }

  detail::backend_flush_result flush() noexcept { return {}; }

  /** @brief token 与请求键共同校验；取消成功后仍保留原完成包责任。 */
  void request_cancel(detail::backend_operation operation) noexcept {
    std::lock_guard lock(mutex_);
    if (!operation.request)
      return;
    auto& r = *static_cast<request_type*>(operation.request);
    auto* node = reinterpret_cast<entry*>(static_cast<std::uintptr_t>(r.native_completion_key));
    if (!node || !node->active || node->token != operation.token || node->cancelled)
      return;
    node->cancelled = true;  // 重复取消不重复提交控制操作，也不改变首次取消原因。
    if ((r.kind == kind::ready || node->polling) && !node->synthetic) {
      remove_ready(*node);                    // readiness 没有内核 OVERLAPPED，先移除观察责任。
      complete_immediate(*node, -ECANCELED);  // 仍经相同最终完成出口回收。
    } else if (!node->synthetic)
      (void)::CancelIoEx(reinterpret_cast<HANDLE>(r.fd), &node->native.overlapped);
    wake();  // ERROR_NOT_FOUND 表示结果可能已排队，不能提前完成/回收。
  }

  /** @brief 批量消费原完成；内核等待不持提交锁，允许远程 submit/cancel。 */
  int poll(std::span<event> output, std::optional<int> timeout) noexcept {
    if (output.empty())
      return 0;
    std::size_t count{};
    bool has_ready{};
    {
      std::lock_guard lock(mutex_);
      scan_ready();                     // 非消费观察；不会抢走业务 recv 的数据。
      count = drain_immediate(output);  // 先交付不会产生内核包的同步成功。
      has_ready = ready_ != nullptr;    // 普通 IO 不承担 readiness 轮询的唤醒开销。
    }
    if (count)
      return static_cast<int>(count);           // skip 成功根本没有对应内核包。
    std::array<OVERLAPPED_ENTRY, 256> packets;  // 有界栈批次，只访问 removed 个有效包。
    ULONG removed{};
    DWORD wait = timeout ? static_cast<DWORD>(std::max(*timeout, 0)) : INFINITE;
    if (has_ready)  // WSAPoll 没有 IOCP 通知，观察者最长等待约 1ms 加调度时间。
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
          wake_pending_.store(false, std::memory_order_release);  // 放开控制通知合并门。
          continue;
        }
        auto* node =
            static_cast<entry*>(reinterpret_cast<native_prefix*>(packet.lpOverlapped)->owner);
        if (!node->active || node->synthetic)  // 重复包意味着内核借用协议被破坏，不能继续复用。
          std::terminate();
        output[count++] = {event_kind::result, node->token, decode(*node, packet), 0};
        --active_;                       // 原请求包已经消费，现在才解除内核完成责任。
        ++statistics_.native_completed;  // 与接受统计形成停机排空证据。
        recycle(*node);                  // 清理 accept 临时句柄后归还稳定池，不释放用户缓冲区。
      }
      scan_ready();                                     // 内核等待期间可能出现新的可读/可写状态。
      count += drain_immediate(output.subspan(count));  // 输出容量不足的结果留在拥有链中。
    }
    return static_cast<int>(count);
  }

  /** @brief 通知包只唤醒控制面；通过原子门合并跨线程通知。 */
  void wake() const noexcept {
    if (!wake_pending_.exchange(true, std::memory_order_acq_rel))
      if (!::PostQueuedCompletionStatus(port_, 0, 0, nullptr))
        std::terminate();
  }

  void begin_shutdown() noexcept { wake(); }  // drain 策略不提前取消 accepted IO。

  bool begin_failure(int) noexcept {
    std::lock_guard lock(mutex_);
    for (auto& owned : entries_) {
      auto& node = *owned;
      if (node.active && !node.synthetic && !node.polling && node.request->kind != kind::ready)
        (void)::CancelIoEx(reinterpret_cast<HANDLE>(node.request->fd), &node.native.overlapped);
    }
    wake();
    return true;  // port 仍存活，原完成责任继续排空。
  }

  bool quiescent() const noexcept {
    std::lock_guard lock(mutex_);
    return active_ == 0;
  }

  detail::backend_statistics statistics() const noexcept {
    std::lock_guard lock(mutex_);
    return statistics_;
  }

 private:
  int attach_locked(native_registration handle) noexcept {
    if (handle.kind != handle_kind::windows_socket && handle.kind != handle_kind::windows_handle)
      return -EINVAL;
    if (auto it = registrations_.find(handle.value); it != registrations_.end())
      return it->second.kind == handle.kind ? 0 : -EINVAL;
    try {
      auto [it, inserted] = registrations_.emplace(handle.value, registration{handle.kind, false});
      (void)inserted;
      HANDLE native =
          reinterpret_cast<HANDLE>(handle.value);  // SOCKET 也是可关联的内核句柄，保留位宽。
      if (!::CreateIoCompletionPort(native, port_, 0, 0)) {
        DWORD error = ::GetLastError();
        registrations_.erase(it);  // 关联失败不接管句柄，也不缓存一个未成功注册的数值。
        return -encode_windows_error(error);
      }
      // 失败保持默认模式：即使 API 同步成功也等待真实完成包。
      it->second.skip =
          ::SetFileCompletionNotificationModes(native, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)
          != FALSE;
      return 0;
    } catch (...) {
      return -ENOMEM;
    }
  }

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

  /** @brief 固定 WSABUF 数组；复制描述符而不复制业务数据，检查 ULONG 溢出。 */
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
    }  // 零向量仍表达零长度数据报。
    return output;
  }

  /** @brief PEEK 和 DONTWAIT 使用非 OVERLAPPED Winsock，绝不在 worker 阻塞。
   * @details PEEK 的非 OVERLAPPED 形式由 Winsock 合同明确支持；DONTWAIT 必须
   *          立即交付 WSAEWOULDBLOCK，不能去掉标志以后提交可能长期挂起的 IO。
   *          数据报 WSAEMSGSIZE 已复制部分字节，按便携 recv 合同交付该进度。
   */
  std::int64_t short_socket(entry& node) noexcept {
    auto& r = *node.request;  // 请求继续保存在域稳定槽，PEEK 重试不建立第二份操作。
    const SOCKET socket = static_cast<SOCKET>(r.fd);
    const DWORD flags = static_cast<DWORD>(r.flags & ~MSG_DONTWAIT);
    if (r.kind == kind::recvmsg) {
      const auto result = recv_message(r.fd,
                                       r.output_message,
                                       r.flags);  // helper 已回写 flags 和实际 copied bytes。
      return result < 0 ? -encode_winsock_error(::WSAGetLastError()) : result;
    }
    if (r.kind == kind::sendmsg || r.kind == kind::sendmsg_zc) {
      r.message.msg_iov = r.vectors.data();  // 拥有型描述符已固定在最终请求中。
      const auto result = send_message(r.fd, &r.message, r.flags);
      return result < 0 ? -encode_winsock_error(::WSAGetLastError()) : result;
    }
    node.scalar[0] = {static_cast<ULONG>(std::min<std::size_t>(r.length, ULONG_MAX)),
                      r.buffer ? static_cast<char*>(r.buffer)
                               : const_cast<char*>(static_cast<const char*>(r.const_buffer))};
    node.flags = flags;  // WSARecv 可以回写截断等 flags，必须保持变量的生命周期。
    node.bytes = 0;
    int status{};
    switch (r.kind) {
      case kind::recv:
        status =
            ::WSARecv(socket, node.scalar.data(), 1, &node.bytes, &node.flags, nullptr, nullptr);
        break;
      case kind::recvfrom:
        node.address_length = sizeof(node.address);  // 完整暂存 peer，完成再按调用方容量复制。
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
      const int error = ::WSAGetLastError();  // 保存原生错误，后续输出转换不能覆盖它。
      if (error != WSAEMSGSIZE || (r.kind != kind::recv && r.kind != kind::recvfrom))
        return -encode_winsock_error(error);
    }
    return node.bytes;  // UDP 截断仍成功交付实际复制字节，包括零长度缓冲区。
  }

  /** @brief 每种业务请求只调用对应的原生 API，PENDING
   * 与同步成功各保留完成责任。 */
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
        complete_immediate(node, 0);  // 流的空读立即成功；UDP 空缓冲仍必须接收/消费数据报。
        return 0;
      }
    }
    if ((receive || transmit) && ((r.flags & MSG_DONTWAIT) || (receive && (r.flags & MSG_PEEK)))) {
      node.short_io = true;  // 消息 helper 已回写输出，finalize 不再覆盖它。
      const auto result = short_socket(node);
      if (result == -encode_winsock_error(WSAEWOULDBLOCK) && !(r.flags & MSG_DONTWAIT)) {
        node.polling = true;  // PEEK 需要等待数据，但不能提交 provider 不保证的
        // OVERLAPPED PEEK。
        node.ready_next = ready_;
        ready_ = &node;  // 保留同一个 stable token 和池节点，取消可以直接摘链。
        return 0;
      }
      if (result < 0)
        return static_cast<int>(-result);  // 非阻塞失败没有内核引用，不接受不存在的完成包。
      complete_immediate(node, result);
      return 0;
    }
    if (r.kind == kind::read || r.kind == kind::write) {
      if (r.native_kind != handle_kind::windows_handle)
        return EINVAL;
      const auto offset =
          r.windows_implicit_cursor ? std::uint64_t{0} : r.offset;  // 非磁盘流没有 seek 游标。
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
      if (r.kind == kind::read && error == ERROR_HANDLE_EOF) {
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
        node.flags |= MSG_TRUNC;  // plain WSARecv 的 finalize 使用该 flags。
        if (!(node.message.dwFlags & (MSG_TRUNC | MSG_CTRUNC)))
          node.message.dwFlags |= MSG_TRUNC;
      }
      complete_immediate(node, node.bytes);  // 即时失败不投递内核包，已经复制的数据仍有效。
      return 0;
    }
    return error == WSA_IO_PENDING ? 0 : encode_winsock_error(error);
  }

  /** @brief 完成以后更新 socket 上下文与输出；取消竞态的 accepted
   * socket仍由域清理。 */
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

  /** @brief 批量 API 的包携带 NTSTATUS；用对应原生 API 还原公开错误域。
   * @details GetQueuedCompletionStatusEx
   * 自身成功只表示取到包，不能当作业务成功。 此处 FALSE
   * 只读取已完成状态，不在驱动线程再次等待。
   */
  std::int64_t decode(entry& node, const OVERLAPPED_ENTRY& packet) noexcept {
    std::int64_t result = packet.dwNumberOfBytesTransferred;  // 成功包直接使用传输量。
    if (packet.Internal != 0) {
      // 失败包必须按文件/Winsock 类型解析，不能使用线程残留 LastError。
      DWORD bytes{}, flags{};
      auto& r = *node.request;
      if (r.native_kind == handle_kind::windows_socket) {
        if (!::WSAGetOverlappedResult(
                static_cast<SOCKET>(r.fd), &node.native.overlapped, &bytes, FALSE, &flags)) {
          int error = ::WSAGetLastError();
          if (error == WSAEMSGSIZE
              && (r.kind == kind::recv || r.kind == kind::recvfrom || r.kind == kind::recvmsg)) {
            if (r.kind == kind::recvmsg) {
              node.flags |= MSG_TRUNC;  // 无地址/control 的 WSARecv 通过 flags 回写。
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
            error == ERROR_HANDLE_EOF && r.kind == kind::read ? 0 : -encode_windows_error(error);
      } else
        result = bytes;
    }
    return finalize(node,
                    result);  // 输出地址/消息头与 accept/connect 上下文只在原完成后更新。
  }

  /** @brief 记录无后续内核包的结果；不调用域回调，不提前复用节点。 */
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

  /** @brief 合成完成和内核完成具有相同的计数、finalize 与回收边界。 */
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

  void remove_ready(entry& node) noexcept {
    auto** link = &ready_;
    while (*link && *link != &node)
      link = &(*link)->ready_next;
    if (*link)
      *link = node.ready_next;
    node.ready_next = nullptr;
  }

  /** @brief 每个观察者独立查询，不合并消费、不执行零字节 recv 或 peek。 */
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
          const auto observed = short_socket(*node);  // 非消费 PEEK，允许假就绪继续保留观察者。
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

  /** @brief 只在拒绝前或最终结果消费后归还池；接受失败的临时 socket 不泄漏。 */
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

  HANDLE port_{};
  mutable std::mutex mutex_;
  mutable std::atomic<bool> wake_pending_{};
  std::vector<std::unique_ptr<entry>> entries_;
  std::unordered_map<std::uintptr_t, registration> registrations_;
  entry *free_{}, *done_{}, *done_tail_{}, *ready_{};
  std::size_t active_{};
  detail::backend_statistics statistics_{};
};

/** @brief 创建 owning 统一后端；初始化失败保留系统错误。 */
inline std::expected<backend_box, std::error_code> make_iocp_backend() noexcept {
  try {
    return backend_box{std::make_unique<iocp_backend>()};
  } catch (const std::system_error& error) {
    return std::unexpected{error.code()};
  } catch (...) {
    return std::unexpected{std::make_error_code(std::errc::not_enough_memory)};
  }
}
}  // namespace faio::io::windows
#endif
