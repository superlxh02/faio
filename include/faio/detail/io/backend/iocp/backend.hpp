#pragma once
/**
 * @file faio/detail/io/backend/iocp/backend.hpp
 * @brief Windows 原生完成端口后端与中立 backend_protocol 的桥接。
 * @details 本文件只负责内核请求与完成包；协程恢复、截止时间和资源方向门
 *          由 io_domain 负责，后端不会保存协程句柄或恢复业务代码。
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
/** @brief 实际 IOCP 能力；路径和目录控制接口使用独立有界文件服务。 */
struct iocp_capabilities {
  static constexpr bool native_proactor = true,  ///< 是否直接提交并接收原生异步操作完成。
      native_filesystem_supported = true;  ///< 该原生后端是否实现文件异步操作。
  static constexpr bool implemented = true,  ///< 当前平台后端是否提供实际实现。
      overlapped_network = true,  ///< 是否支持 Windows 重叠网络操作。
      overlapped_files = true,  ///< 是否支持 Windows 重叠文件操作。
      readiness = true;  ///< 是否提供读写就绪观察能力。
};

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
  using request_type = detail::io_request;  ///< 该后端或协议使用的统一请求描述类型。
  using kind = detail::operation_kind;  ///< 当前请求、事件或句柄的种类标签。
  using event = detail::backend_event;  ///< 该后端对外产生的统一完成事件类型。
  using event_kind = detail::backend_event_kind;  ///< 用于区分业务完成、就绪与控制回执的事件种类。
  using handle_kind = detail::native_handle_kind;  ///< 区分 POSIX 描述符、Windows socket 与文件句柄的标签类型。
  struct entry;
  using native_prefix = overlapped_operation_state;  ///< 标准布局前缀允许从原生 OVERLAPPED 安全恢复池节点。

  struct entry {
    native_prefix native;  ///< 标准布局 OVERLAPPED 前缀，内核借用地址不变。
    std::uint64_t token{};  ///< 域操作的完整槽位和代际，防止迟到取消误伤复用槽。
    request_type* request{};  ///< 只借用已接受域请求，最终完成前域不得释放它。
    entry *next{},  ///< 侵入式任务队列后继，不拥有后继节点。
        *ready_next{};  ///< 就绪条目链表后继，与完成链表使用独立链接。
    std::array<WSABUF, 2> scalar{};  ///< 单次读写使用的内联缓冲区向量，避免通用堆分配。
    std::vector<WSABUF> vectors;  ///< 向量容量随节点复用，只复制描述符。
    WSAMSG message{};  ///< 稳定保存的消息描述及缓冲区引用，安全完成前不得迁移。
    sockaddr_storage address{};  ///< 拥有的本地或对端 socket 地址，提交期间不借用临时对象。
    int address_length{};  ///< 原生 socket 地址的有效字节长度。
    std::array<std::byte, 2 * (sizeof(sockaddr_storage) + 16)> accept_addresses{};  ///< AcceptEx 使用的本地与远端地址存储，保持到重叠操作完成。
    SOCKET accepted{INVALID_SOCKET};  ///< AcceptEx 接受的临时拥有句柄，交付前失败由池清理。
    DWORD bytes{},  ///< 原生重叠操作实际传输的字节数，完成解码时交给统一协议。
        flags{};  ///< 本次原生操作的标志位，按对应系统调用语义解释。
    std::int64_t immediate{};  ///< 仅保存不会再产生内核包的同步结果。
    bool active{},  ///< 记录尚未安全完成的文件操作，关闭与同步查询据此等待排空。
        synthetic{},  ///< 本条目是否由后端内部合成完成，区别于真实内核回执。
        skip{},  ///< 是否跳过本次原生动作，由外层状态仲裁决定。
        cancelled{};  ///< 本次操作是否以取消原因结束。
    bool short_io{},  ///< 本次重叠传输是否已取得部分字节进度。
        polling{};  ///< 该条目是否处于就绪观察或轮询路径。
    /// @brief PEEK/DONTWAIT 用短非阻塞调用；PEEK
    /// @brief 的等待责任由观察链保留。
  };

  struct registration {
    handle_kind kind;  ///< 当前请求、事件或句柄的种类标签。
    bool skip;  ///< 是否跳过本次原生动作，由外层状态仲裁决定。
  };

 public:
  static constexpr bool native_proactor = true,  ///< 是否直接提交并接收原生异步操作完成。
      poll_flushes_submissions = true;  ///< poll 是否负责冲刷待提交请求，避免驱动器重复提交。
  static constexpr const char* backend_name = "iocp";  ///< 后端名称常量，用于能力报告与诊断。

  /** @brief 预热稳定池，再创建完成端口；构造阶段不接受业务请求。 */
  explicit iocp_backend(std::size_t capacity = 4096) {
    if (auto network = initialize_winsock(); !network)
      throw std::system_error(network.error().value(), std::system_category(), "WSAStartup");
    /// @brief 所有者容器的地址不被内核借用。
    entries_.reserve(capacity);
    for (std::size_t i = 0; i < capacity; ++i) {
      auto node = std::make_unique<entry>();
      /// @brief 内核前缀和真实节点身份固定到最终包。
      node->native.owner = node.get();
      /// @brief 使用节点内链表，提交热路径不分配队列节点。
      node->next = free_;
      /// @brief 只有未被内核借用的节点进入空闲链。
      free_ = node.get();
      /// @brief unique_ptr 转移不移动节点本身。
      entries_.push_back(std::move(node));
    }
    /// @brief 每域一个完成消费者。
    port_ = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0,
                                     1);
    if (!port_)
      throw std::system_error(::GetLastError(), std::system_category(), "CreateIoCompletionPort");
  }

  iocp_backend(const iocp_backend&) = delete;

  ~iocp_backend() {
    /// @brief 宿主必须先排空，绝不能释放内核仍借用的状态。
    if (active_)
      std::terminate();
    if (port_)
      ::CloseHandle(port_);
  }

  /// @brief 返回实际后端的静态诊断名称。
  const char* name() const noexcept { return backend_name; }

  /** @brief 持久关联完整位宽句柄；失败时不接管调用方的所有权。 */
  int register_resource(native_registration handle, std::uint64_t) noexcept {
    /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
    std::lock_guard lock(mutex_);
    return register_resource_locked(handle);
  }

  /** @brief IOCP 关联不能解除；删除用户态缓存，实际关闭仍由资源负责。 */
  void unregister_resource(native_registration handle) noexcept {
    /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
    std::lock_guard lock(mutex_);
    registrations_.erase(handle.value);
  }

  /// @brief 查询初始化时固定的原生能力，避免普通请求热路径重复探测平台。
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
        /// @brief 无异步原生接口的控制操作由 domain 的服务 lane 执行。
        return false;
    }
  }

  /** @brief 接受前取得稳定节点；同步失败不保留任何内核 payload 引用。 */
  submit_result try_submit(detail::backend_operation operation) noexcept {
    /// @brief 节点取得、提交与取消共用短锁。
    std::lock_guard lock(mutex_);
    /// @brief 后端协议只传稳定请求地址。
    auto& r = *static_cast<request_type*>(operation.request);
    if (!supports(static_cast<std::uint32_t>(r.kind)) && r.kind != kind::send_zc
        && r.kind != kind::sendmsg_zc)
      return {submit_status::rejected, EOPNOTSUPP};
    /// @brief 有界背压在接受前返回；域仍拥有尚未提交的请求。
    if (!free_)
      return {submit_status::would_queue, 0};
    /// @brief HANDLE/SOCKET 保存为完整位宽，-1 是统一的无效标记。
    if (r.fd < 0)
      return {submit_status::rejected, EBADF};
    const native_registration handle{static_cast<std::uintptr_t>(r.fd), r.native_kind};
    if (int error = register_resource_locked(handle); error)
      return {submit_status::rejected, -error};
    /// @brief 此节点直到原生结果消费后才允许再次进入空闲链。
    auto* node = free_;
    /// @brief 取得稳定节点，避免内核借用可扩容 vector 元素。
    free_ = node->next;
    /// @brief 清除前一代的空闲链链接。
    node->next = nullptr;
    /// @brief 原生事件和偏移不会继承前一代状态。
    node->native.overlapped = {};
    /// @brief 取消时必须同时匹配请求键和完整代际。
    node->token = operation.token;
    /// @brief 参数、地址和借用缓冲区都由域的稳定槽保活。
    node->request = &r;
    /// @brief 初始化本代原生输出字段，防止复用条目沿用上次传输进度或消息标志。
    node->bytes = node->flags = 0;
    /// @brief 清空本代原生地址输出存储，只在真实成功后解释其有效范围。
    node->address = {};
    /// @brief 预置完整地址输出容量，内核完成时再写入实际地址长度。
    node->address_length = sizeof(sockaddr_storage);
    /// @brief 初始化本代 WSAMSG，随后只填入当前请求需要的地址、负载及控制数据引用。
    node->message = {};
    /// @brief 本代尚未接管临时 accept socket，失败清理不能关闭上代已经交付的句柄。
    node->accepted = INVALID_SOCKET;
    /// @brief 标记条目已被本次提交占用，取消入口仍须同时匹配完整操作代际。
    node->active = true;
    /// @brief 清除上一代合成完成与取消状态，新请求分别建立自己的完成责任。
    node->synthetic = node->cancelled = false;
    /// @brief 每代重置，不继承上一个观察请求的模式。
    node->short_io = node->polling = false;
    /// @brief 每句柄保存实际跳包设置结果。
    node->skip = registrations_.at(handle.value).skip;
    /// @brief 取消 O(1) 找到原 OVERLAPPED。
    r.native_completion_key =
        reinterpret_cast<std::uintptr_t>(node);
    try {
      if (int error = issue(*node); error) {
        /// @brief 同步失败没有内核借用；临时 accept 句柄一并清理。
        recycle(*node);
        /// @brief 被拒绝的请求不能留下可供取消的旧地址。
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
    /// @brief 已接受的原生/合成结果都承担一次最终完成责任。
    ++active_;
    /// @brief 统计接受数，不统计 rejected/would_queue。
    ++statistics_.native_submitted;
    /// @brief IOCP 原 API 即提交，无 Linux 式延迟 enter。
    ++statistics_.native_flushed;
    /// @brief IOCP 在原 API 中提交，无额外 flush 队列。
    return {submit_status::accepted, 0};
  }

  /// @brief 提交缓冲的写入或已准备的后端请求，失败保留尚未完成的责任。
  detail::backend_flush_result flush() noexcept { return {}; }

  /** @brief token 与请求键共同校验；取消成功后仍保留原完成包责任。 */
  void request_cancel(detail::backend_operation operation) noexcept {
    /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
    std::lock_guard lock(mutex_);
    if (!operation.request)
      return;
    auto& r = *static_cast<request_type*>(operation.request);
    auto* node = reinterpret_cast<entry*>(static_cast<std::uintptr_t>(r.native_completion_key));
    if (!node || !node->active || node->token != operation.token || node->cancelled)
      return;
    /// @brief 重复取消不重复提交控制操作，也不改变首次取消原因。
    node->cancelled = true;
    if ((r.kind == kind::ready || node->polling) && !node->synthetic) {
      /// @brief readiness 没有内核 OVERLAPPED，先移除观察责任。
      remove_ready(*node);
      /// @brief 仍经相同最终完成出口回收。
      complete_immediate(*node, -ECANCELED);
    } else if (!node->synthetic)
      /// @brief 按原 OVERLAPPED 身份请求取消；调用成功也必须等待原操作最终完成包。
      (void)::CancelIoEx(reinterpret_cast<HANDLE>(r.fd), &node->native.overlapped);
    /// @brief ERROR_NOT_FOUND 表示结果可能已排队，不能提前完成/回收。
    wake();
  }

  /** @brief 批量消费原完成；内核等待不持提交锁，允许远程 submit/cancel。 */
  int poll(std::span<event> output, std::optional<int> timeout) noexcept {
    /// @brief 调用方没有事件输出容量时不消费完成队列，也不进入内核等待。
    if (output.empty())
      return 0;
    std::size_t count{};
    bool has_ready{};
    {
      /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
      std::lock_guard lock(mutex_);
      /// @brief 非消费观察；不会抢走业务 recv 的数据。
      scan_ready();
      /// @brief 先交付不会产生内核包的同步成功。
      count = drain_immediate(output);
      /// @brief 普通 IO 不承担 readiness 轮询的唤醒开销。
      has_ready = ready_ != nullptr;
    }
    /// @brief skip 成功根本没有对应内核包。
    if (count)
      /// @brief 只返回实际填入调用方输出区的事件数量，未消费事件仍留给后续驱动。
      return static_cast<int>(count);
    /// @brief 有界栈批次，只访问 removed 个有效包。
    std::array<OVERLAPPED_ENTRY, 256> packets;
    ULONG removed{};
    DWORD wait = timeout ? static_cast<DWORD>(std::max(*timeout, 0)) : INFINITE;
    /// @brief WSAPoll 没有 IOCP 通知，观察者最长等待约 1ms 加调度时间。
    if (has_ready)
      wait = std::min<DWORD>(wait, 1);
    /// @brief 以调用方预算批量取得真实 IOCP 完成包，控制键与业务节点分别解码。
    if (!::GetQueuedCompletionStatusEx(port_,
                                       packets.data(),
                                       static_cast<ULONG>(std::min(output.size(), packets.size())),
                                       &removed,
                                       wait,
                                       FALSE)) {
      /// @brief 原生失败后立即保存 Win32 错误，后续清理与输出转换不能覆盖该值。
      DWORD error = ::GetLastError();
      if (error != WAIT_TIMEOUT)
        return -encode_windows_error(error);
    }
    {
      /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
      std::lock_guard lock(mutex_);
      for (ULONG i = 0; i < removed; ++i) {
        auto& packet = packets[i];
        if (!packet.lpOverlapped) {
          /// @brief 放开控制通知合并门。
          wake_pending_.store(false, std::memory_order_release);
          continue;
        }
        auto* node =
            static_cast<entry*>(reinterpret_cast<native_prefix*>(packet.lpOverlapped)->owner);
        /// @brief 重复包意味着内核借用协议被破坏，不能继续复用。
        if (!node->active || node->synthetic)
          std::terminate();
        output[count++] = {event_kind::result, node->token, decode(*node, packet), 0};
        /// @brief 原请求包已经消费，现在才解除内核完成责任。
        --active_;
        /// @brief 与接受统计形成停机排空证据。
        ++statistics_.native_completed;
        /// @brief 清理 accept 临时句柄后归还稳定池，不释放用户缓冲区。
        recycle(*node);
      }
      /// @brief 内核等待期间可能出现新的可读/可写状态。
      scan_ready();
      /// @brief 输出容量不足的结果留在拥有链中。
      count += drain_immediate(output.subspan(count));
    }
    /// @brief 只返回实际填入调用方输出区的事件数量，未消费事件仍留给后续驱动。
    return static_cast<int>(count);
  }

  /** @brief 通知包只唤醒控制面；通过原子门合并跨线程通知。 */
  void wake() const noexcept {
    if (!wake_pending_.exchange(true, std::memory_order_acq_rel))
      /// @brief 通过专用完成键发布控制唤醒，不能代替业务请求的真实完成。
      if (!::PostQueuedCompletionStatus(port_, 0, 0, nullptr))
        std::terminate();
  }

  /// @brief drain 策略不提前取消 accepted IO。
  void begin_shutdown() noexcept { wake(); }

  /// @brief 处理永久后端故障，仅在能够保证原生引用安全解除时转入失败排空。
  bool begin_failure(int) noexcept {
    /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
    std::lock_guard lock(mutex_);
    for (auto& owned : entries_) {
      auto& node = *owned;
      if (node.active && !node.synthetic && !node.polling && node.request->kind != kind::ready)
        /// @brief 按原 OVERLAPPED 身份请求取消；调用成功也必须等待原操作最终完成包。
        (void)::CancelIoEx(reinterpret_cast<HANDLE>(node.request->fd), &node.native.overlapped);
    }
    wake();
    /// @brief port 仍存活，原完成责任继续排空。
    return true;
  }

  /// @brief 检查当前组件是否已经没有尚未归还的执行、内核引用与完成责任。
  bool quiescent() const noexcept {
    /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
    std::lock_guard lock(mutex_);
    return active_ == 0;
  }

  /// @brief 读取后端累计提交、完成与控制事件统计。
  detail::backend_statistics statistics() const noexcept {
    /// @brief 以组件短锁串行保护共享状态；可能等待内核或执行用户回调的动作在所属协议允许的锁外完成。
    std::lock_guard lock(mutex_);
    return statistics_;
  }

 private:
  /// @brief 在后端锁内把完整宽度原生句柄关联到完成端口。
  int register_resource_locked(native_registration handle) noexcept {
    if (handle.kind != handle_kind::windows_socket && handle.kind != handle_kind::windows_handle)
      return -EINVAL;
    if (auto it = registrations_.find(handle.value); it != registrations_.end())
      return it->second.kind == handle.kind ? 0 : -EINVAL;
    try {
      auto [it, inserted] = registrations_.emplace(handle.value, registration{handle.kind, false});
      (void)inserted;
      /// @brief SOCKET 也是可关联的内核句柄，保留位宽。
      HANDLE native =
          reinterpret_cast<HANDLE>(handle.value);
      /// @brief 把完整宽度原生句柄关联到本后端完成端口，关联不能在普通 IO 热路径反复迁移。
      if (!::CreateIoCompletionPort(native, port_, 0, 0)) {
        /// @brief 原生失败后立即保存 Win32 错误，后续清理与输出转换不能覆盖该值。
        DWORD error = ::GetLastError();
        /// @brief 关联失败不接管句柄，也不缓存一个未成功注册的数值。
        registrations_.erase(it);
        return -encode_windows_error(error);
      }
      /// @brief 失败保持默认模式：即使 API 同步成功也等待真实完成包。
      it->second.skip =
          ::SetFileCompletionNotificationModes(native, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)
          != FALSE;
      return 0;
    } catch (...) {
      return -ENOMEM;
    }
  }

  /// @brief 按 socket 提供者查询 Windows 扩展函数入口，失败保留原生错误。
  template <class Function>
  static int extension(SOCKET socket, GUID id, Function& output) noexcept {
    DWORD bytes{};
    /// @brief 立即读取 Winsock 错误，后续系统调用不得覆盖本次失败原因。
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
    /// @brief 本批没有业务完成时只推进必要清理重试，然后结束本次完成领取。
    if (!count) {
      output[0] = {0, nullptr};
      count = 1;
    /// @brief 零向量仍表达零长度数据报。
    }
    return output;
  }

  /** @brief PEEK 和 DONTWAIT 使用非 OVERLAPPED Winsock，绝不在 worker 阻塞。
   * @details PEEK 的非 OVERLAPPED 形式由 Winsock 合同明确支持；DONTWAIT 必须
   *          立即交付 WSAEWOULDBLOCK，不能去掉标志以后提交可能长期挂起的 IO。
   *          数据报 WSAEMSGSIZE 已复制部分字节，按便携 recv 合同交付该进度。
   */
  std::int64_t short_socket(entry& node) noexcept {
    /// @brief 请求继续保存在域稳定槽，PEEK 重试不建立第二份操作。
    auto& r = *node.request;
    const SOCKET socket = static_cast<SOCKET>(r.fd);
    const DWORD flags = static_cast<DWORD>(r.flags & ~MSG_DONTWAIT);
    if (r.kind == kind::recvmsg) {
      /// @brief helper 已回写 flags 和实际 copied bytes。
      const auto result = recv_message(r.fd,
                                       r.output_message,
                                       r.flags);
      /// @brief 立即读取 Winsock 错误，后续系统调用不得覆盖本次失败原因。
      return result < 0 ? -encode_winsock_error(::WSAGetLastError()) : result;
    }
    if (r.kind == kind::sendmsg || r.kind == kind::sendmsg_zc) {
      /// @brief 拥有型描述符已固定在最终请求中。
      r.message.msg_iov = r.vectors.data();
      const auto result = send_message(r.fd, &r.message, r.flags);
      /// @brief 立即读取 Winsock 错误，后续系统调用不得覆盖本次失败原因。
      return result < 0 ? -encode_winsock_error(::WSAGetLastError()) : result;
    }
    node.scalar[0] = {static_cast<ULONG>(std::min<std::size_t>(r.length, ULONG_MAX)),
                      r.buffer ? static_cast<char*>(r.buffer)
                               : const_cast<char*>(static_cast<const char*>(r.const_buffer))};
    /// @brief WSARecv 可以回写截断等 flags，必须保持变量的生命周期。
    node.flags = flags;
    node.bytes = 0;
    int status{};
    switch (r.kind) {
      case kind::recv:
        /// @brief 调用原生接收接口，输出字节数与标志保存在稳定节点，负载借用覆盖操作安全完成期。
        status =
            ::WSARecv(socket, node.scalar.data(), 1, &node.bytes, &node.flags, nullptr, nullptr);
        break;
      case kind::recvfrom:
        /// @brief 完整暂存 peer，完成再按调用方容量复制。
        node.address_length = sizeof(node.address);
        /// @brief 调用原生报文接收接口，同时保留对端地址输出；零长度报文仍有独立报文语义。
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
        /// @brief 调用原生发送接口，只借用提交的负载；成功数量仍可能小于调用方完整数据长度。
        status = ::WSASend(socket, node.scalar.data(), 1, &node.bytes, flags, nullptr, nullptr);
        break;
      case kind::sendto:
        /// @brief 调用原生定向报文发送接口，目标地址与负载均由稳定请求覆盖完成期。
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
    /// @brief 原生调用同步报错时立即查询错误；已接纳的异步等待仍须保留稳定条目。
    if (status == SOCKET_ERROR) {
      /// @brief 保存原生错误，后续输出转换不能覆盖它。
      const int error = ::WSAGetLastError();
      if (error != WSAEMSGSIZE || (r.kind != kind::recv && r.kind != kind::recvfrom))
        return -encode_winsock_error(error);
    }
    /// @brief UDP 截断仍成功交付实际复制字节，包括零长度缓冲区。
    return node.bytes;
  }

  /** @brief 每种业务请求只调用对应的原生 API，PENDING
   * 与同步成功各保留完成责任。 */
  int issue(entry& node) {
    /// @brief 节点只借用域中已经接纳且地址稳定的请求，最终原生完成前域不会回收该请求。
    auto& r = *node.request;
    /// @brief 取得节点内嵌 OVERLAPPED 的稳定地址，所有重叠调用和完成解码使用同一身份。
    auto* overlapped = &node.native.overlapped;
    /// @brief 按原生 SOCKET 完整宽度还原句柄，不能经过 int 截断。
    SOCKET socket = static_cast<SOCKET>(r.fd);
    /// @brief 先设失败哨兵，各原生调用分支必须明确写入实际提交结果。
    int status = SOCKET_ERROR;
    /// @brief 纯就绪观察进入后端观察链，不伪造任何数据传输结果。
    if (r.kind == kind::ready) {
      /// @brief 将稳定节点头插到就绪观察链，使用独立链接避免破坏其他完成链。
      node.ready_next = ready_;
      /// @brief 发布新的观察链头，取消或轮询路径以后仍按相同节点身份摘除。
      ready_ = &node;
      return 0;
    }
    const bool receive =
        r.kind == kind::recv || r.kind == kind::recvfrom || r.kind == kind::recvmsg;
    const bool transmit = r.kind == kind::send || r.kind == kind::send_zc || r.kind == kind::sendto
                          || r.kind == kind::sendmsg || r.kind == kind::sendmsg_zc;
    /// @brief 空接收需要区分字节流无操作与必须消费零长报文的数据报语义。
    if (r.kind == kind::recv && !r.length) {
      int type{}, size = sizeof(type);
      if (::getsockopt(socket, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &size))
        /// @brief 立即读取 Winsock 错误，后续系统调用不得覆盖本次失败原因。
        return encode_winsock_error(::WSAGetLastError());
      /// @brief 只有字节流空接收才允许立即成功，不能把数据报接收跳过。
      if (type == SOCK_STREAM) {
        /// @brief 流的空读立即成功；UDP 空缓冲仍必须接收/消费数据报。
        complete_immediate(node, 0);
        return 0;
      }
    }
    if ((receive || transmit) && ((r.flags & MSG_DONTWAIT) || (receive && (r.flags & MSG_PEEK)))) {
      /// @brief 消息 helper 已回写输出，finalize 不再覆盖它。
      node.short_io = true;
      /// @brief 对显式非阻塞或窥探请求先执行真实短调用，保留实际字节数和原生错误。
      const auto result = short_socket(node);
      if (result == -encode_winsock_error(WSAEWOULDBLOCK) && !(r.flags & MSG_DONTWAIT)) {
        /// @brief PEEK 需要等待数据，但不能提交 provider 不保证的 OVERLAPPED PEEK。
        node.polling = true;

        /// @brief 将稳定节点头插到就绪观察链，使用独立链接避免破坏其他完成链。
        node.ready_next = ready_;
        /// @brief 保留同一个 stable token 和池节点，取消可以直接摘链。
        ready_ = &node;
        return 0;
      }
      /// @brief 非阻塞失败没有内核引用，不接受不存在的完成包。
      if (result < 0)
        return static_cast<int>(-result);
      /// @brief 短调用已真实结束，将其结果作为一次已接受操作的即时完成。
      complete_immediate(node, result);
      return 0;
    }
    /// @brief 文件读写通过真实 ReadFile 或 WriteFile 重叠接口执行，区别于 socket 收发。
    if (r.kind == kind::read || r.kind == kind::write) {
      /// @brief 文件 API 只能接收文件句柄种类，错误类型在发起原生调用前拒绝。
      if (r.native_kind != handle_kind::windows_handle)
        return EINVAL;
      /// @brief 非磁盘流没有 seek 游标。
      const auto offset =
          r.windows_implicit_cursor ? std::uint64_t{0} : r.offset;
      /// @brief 把 64 位文件偏移低半部写入原生重叠结构。
      overlapped->Offset = static_cast<DWORD>(offset);
      /// @brief 写入偏移高半部，避免大文件定位被 32 位字段截断。
      overlapped->OffsetHigh = static_cast<DWORD>(offset >> 32);
      /// @brief 单次原生文件 IO 长度受 DWORD 限制，超过部分由上层短读写循环继续处理。
      DWORD length = static_cast<DWORD>(std::min<std::size_t>(r.length, MAXDWORD));
      /// @brief 通过真实原生文件接口发起读取，OVERLAPPED、输出数量及借用缓冲区保持到最终完成。
      BOOL success =
          r.kind == kind::read
              ? ::ReadFile(
                    reinterpret_cast<HANDLE>(r.fd), r.buffer, length, &node.bytes, overlapped)
              : ::WriteFile(reinterpret_cast<HANDLE>(r.fd),
                            r.const_buffer,
                            length,
                            &node.bytes,
                            overlapped);
      /// @brief 同步成功也可能产生完成包，是否合成即时结果须服从该句柄的跳包配置。
      if (success) {
        /// @brief 只有该句柄确实启用了同步成功跳包，才由后端自行交付一次即时完成。
        if (node.skip)
          /// @brief 把同步成功的实际字节数交给内部完成队列，避免因跳过原生完成包丢失交付。
          complete_immediate(node, node.bytes);
        return 0;
      }
      /// @brief 原生失败后立即保存 Win32 错误，后续清理与输出转换不能覆盖该值。
      DWORD error = ::GetLastError();
      /// @brief 请求已经被内核接纳；必须保留 OVERLAPPED 与负载，等待最终完成包。
      if (error == ERROR_IO_PENDING)
        return 0;
      /// @brief 原生文件读取命中 EOF 时转为成功零读，避免把正常文件结束当作异常。
      if (r.kind == kind::read && error == ERROR_HANDLE_EOF) {
        /// @brief 后端已承担完成责任，以内部即时队列交付零结果，避免等待不会到来的原生包。
        complete_immediate(node, 0);
        return 0;
      }
      /// @brief 给错误附加 Win32 错误域标签，统一完成层才能正确恢复错误类别。
      return encode_windows_error(error);
    }
    /// @brief 网络 API 只接受 socket 种类，不允许把普通 HANDLE 解释成 SOCKET。
    if (r.native_kind != handle_kind::windows_socket)
      return EINVAL;
    /// @brief 连接请求按地址族准备本地绑定，再通过当前提供者的 ConnectEx 扩展发起。
    if (r.kind == kind::connect) {
      int type{}, length = sizeof(type);
      if (::getsockopt(socket, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &length))
        /// @brief 立即读取 Winsock 错误，后续系统调用不得覆盖本次失败原因。
        return encode_winsock_error(::WSAGetLastError());
      /// @brief 数据报 connect 只设置默认对端，不需要套用字节流重叠连接协议。
      if (type == SOCK_DGRAM) {
        if (::connect(socket, reinterpret_cast<sockaddr*>(&r.address), r.address_length))
          /// @brief 立即读取 Winsock 错误，后续系统调用不得覆盖本次失败原因。
          return encode_winsock_error(::WSAGetLastError());
        /// @brief 后端已承担完成责任，以内部即时队列交付零结果，避免等待不会到来的原生包。
        complete_immediate(node, 0);
        return 0;
      }
      /// @brief 保存为当前 socket 提供者查询出的 ConnectEx 入口，不假定所有提供者共享函数地址。
      LPFN_CONNECTEX function{};
      /// @brief 以系统定义的扩展标识查询真正的异步连接入口。
      GUID id = WSAID_CONNECTEX;
      if (int error = extension(socket, id, function); error)
        return error;
      /// @brief 为 ConnectEx 所需的本地绑定或接受连接的地址族查询保留完整原生地址存储。
      sockaddr_storage local{};
      int size = sizeof(local);
      if (::getsockname(socket, reinterpret_cast<sockaddr*>(&local), &size) == SOCKET_ERROR) {
        /// @brief 尚未绑定的 socket 使用目标地址族建立通配本地地址。
        local.ss_family = r.address.ss_family;
        /// @brief 按实际地址族选择绑定长度，不能把 sockaddr_storage 全容量当作有效地址长度。
        size = local.ss_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
        if (::bind(socket, reinterpret_cast<sockaddr*>(&local), size))
          /// @brief 立即读取 Winsock 错误，后续系统调用不得覆盖本次失败原因。
          return encode_winsock_error(::WSAGetLastError());
      }
      BOOL success = function(socket,
                              reinterpret_cast<sockaddr*>(&r.address),
                              r.address_length,
                              nullptr,
                              0,
                              &node.bytes,
                              overlapped);
      /// @brief 同步成功也可能产生完成包，是否合成即时结果须服从该句柄的跳包配置。
      if (success) {
        /// @brief 只有该句柄确实启用了同步成功跳包，才由后端自行交付一次即时完成。
        if (node.skip)
          /// @brief 后端已承担完成责任，以内部即时队列交付零结果，避免等待不会到来的原生包。
          complete_immediate(node, 0);
        return 0;
      }
      /// @brief 立即捕获 Winsock 原生错误，并区分等待完成与同步失败。
      int error = ::WSAGetLastError();
      /// @brief PENDING 表示接纳成功且保留完成责任；其他错误才可按接受前失败返回。
      return error == WSA_IO_PENDING ? 0 : encode_winsock_error(error);
    }
    /// @brief 接受连接使用独立重叠 socket 与稳定地址缓冲区，失败路径须回收临时句柄。
    if (r.kind == kind::accept) {
      /// @brief 保存当前监听 socket 提供者的 AcceptEx 入口。
      LPFN_ACCEPTEX function{};
      /// @brief 以 AcceptEx 的扩展标识查询当前提供者支持的原生接受实现。
      GUID id = WSAID_ACCEPTEX;
      if (int error = extension(socket, id, function); error)
        return error;
      /// @brief 为 ConnectEx 所需的本地绑定或接受连接的地址族查询保留完整原生地址存储。
      sockaddr_storage local{};
      int size = sizeof(local);
      if (::getsockname(socket, reinterpret_cast<sockaddr*>(&local), &size))
        /// @brief 立即读取 Winsock 错误，后续系统调用不得覆盖本次失败原因。
        return encode_winsock_error(::WSAGetLastError());
      /// @brief 创建支持重叠 IO 且禁止继承的新 socket；失败前取得的临时句柄仍由后端回收。
      node.accepted = ::WSASocketW(local.ss_family,
                                   SOCK_STREAM,
                                   IPPROTO_TCP,
                                   nullptr,
                                   0,
                                   WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
      /// @brief 临时接收 socket 创建失败时，尚未发起 AcceptEx，直接保留原生错误。
      if (node.accepted == INVALID_SOCKET)
        /// @brief 立即读取 Winsock 错误，后续系统调用不得覆盖本次失败原因。
        return encode_winsock_error(::WSAGetLastError());
      BOOL success = function(socket,
                              node.accepted,
                              node.accept_addresses.data(),
                              0,
                              sizeof(sockaddr_storage) + 16,
                              sizeof(sockaddr_storage) + 16,
                              &node.bytes,
                              overlapped);
      /// @brief 同步成功也可能产生完成包，是否合成即时结果须服从该句柄的跳包配置。
      if (success) {
        /// @brief 只有该句柄确实启用了同步成功跳包，才由后端自行交付一次即时完成。
        if (node.skip)
          /// @brief 后端已承担完成责任，以内部即时队列交付零结果，避免等待不会到来的原生包。
          complete_immediate(node, 0);
        return 0;
      }
      /// @brief 立即捕获 Winsock 原生错误，并区分等待完成与同步失败。
      int error = ::WSAGetLastError();
      /// @brief PENDING 表示接纳成功且保留完成责任；其他错误才可按接受前失败返回。
      return error == WSA_IO_PENDING ? 0 : encode_winsock_error(error);
    }
    if (r.kind == kind::recvmsg || r.kind == kind::sendmsg || r.kind == kind::sendmsg_zc) {
      auto& message = r.kind == kind::recvmsg ? *r.output_message : r.message;
      auto* vectors = r.kind == kind::recvmsg ? message.msg_iov : r.vectors.data();
      auto size = r.kind == kind::recvmsg ? message.msg_iovlen : r.vectors.size();
      /// @brief 将分散聚集描述转换为稳定 WSABUF 数组，数量与数组一起保存到节点。
      node.message.lpBuffers = buffers(node, node.message.dwBufferCount, vectors, size);
      /// @brief 只借用调用方地址输出区，外层请求租约必须覆盖整个原生完成期。
      node.message.name = static_cast<sockaddr*>(message.msg_name);
      /// @brief 把地址输出容量转换为原生消息输入长度，完成后再回填真实长度。
      node.message.namelen = message.msg_namelen;
      /// @brief 控制消息长度必须能放入 Windows 原生字段，不能静默窄化截断。
      if (message.msg_controllen > ULONG_MAX)
        return EINVAL;
      node.message.Control = {static_cast<ULONG>(message.msg_controllen),
                              static_cast<char*>(message.msg_control)};
      /// @brief MSG_DONTWAIT 已由短调用路径处理，重叠提交不向提供者传递该模拟标志。
      node.message.dwFlags = static_cast<DWORD>(r.flags & ~MSG_DONTWAIT);
      if (r.kind == kind::recvmsg && !message.msg_name && !message.msg_control) {
        /// @brief 接收标志保存在稳定节点中，原生完成前不能使用短生命期局部变量作为输出。
        node.flags = node.message.dwFlags;
        /// @brief 调用原生接收接口，输出字节数与标志保存在稳定节点，负载借用覆盖操作安全完成期。
        status = ::WSARecv(socket,
                           node.message.lpBuffers,
                           node.message.dwBufferCount,
                           &node.bytes,
                           &node.flags,
                           overlapped,
                           nullptr);
      } else if (r.kind != kind::recvmsg && !message.msg_control) {
        /// @brief 调用原生定向报文发送接口，目标地址与负载均由稳定请求覆盖完成期。
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
          /// @brief 调用原生接收接口，输出字节数与标志保存在稳定节点，负载借用覆盖操作安全完成期。
          status = ::WSARecv(
              socket, node.scalar.data(), 1, &node.bytes, &node.flags, overlapped, nullptr);
          break;
        case kind::send:
        case kind::send_zc:
          /// @brief 调用原生发送接口，只借用提交的负载；成功数量仍可能小于调用方完整数据长度。
          status = ::WSASend(
              socket, node.scalar.data(), 1, &node.bytes, node.flags, overlapped, nullptr);
          break;
        case kind::recvfrom:
          /// @brief 调用原生报文接收接口，同时保留对端地址输出；零长度报文仍有独立报文语义。
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
          /// @brief 调用原生定向报文发送接口，目标地址与负载均由稳定请求覆盖完成期。
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
      /// @brief 只有该句柄确实启用了同步成功跳包，才由后端自行交付一次即时完成。
      if (node.skip)
        /// @brief 把同步成功的实际字节数交给内部完成队列，避免因跳过原生完成包丢失交付。
        complete_immediate(node, node.bytes);
      return 0;
    }
    /// @brief 立即捕获 Winsock 原生错误，并区分等待完成与同步失败。
    int error = ::WSAGetLastError();
    if (error == WSAEMSGSIZE && receive) {
      if (r.kind == kind::recvmsg) {
        /// @brief plain WSARecv 的 finalize 使用该 flags。
        node.flags |= MSG_TRUNC;
        if (!(node.message.dwFlags & (MSG_TRUNC | MSG_CTRUNC)))
          node.message.dwFlags |= MSG_TRUNC;
      }
      /// @brief 即时失败不投递内核包，已经复制的数据仍有效。
      complete_immediate(node, node.bytes);
      return 0;
    }
    /// @brief PENDING 表示接纳成功且保留完成责任；其他错误才可按接受前失败返回。
    return error == WSA_IO_PENDING ? 0 : encode_winsock_error(error);
  }

  /** @brief 完成以后更新 socket 上下文与输出；取消竞态的 accepted
   * socket仍由域清理。 */
  std::int64_t finalize(entry& node, std::int64_t result) noexcept {
    /// @brief 节点只借用域中已经接纳且地址稳定的请求，最终原生完成前域不会回收该请求。
    auto& r = *node.request;
    /// @brief 短调用失败且没有内核借用时，可在接受前按原生错误拒绝请求。
    if (result < 0)
      return result;
    if (node.short_io
        && (r.kind == kind::recvmsg || r.kind == kind::sendmsg || r.kind == kind::sendmsg_zc))
      return result;
    /// @brief 按原生 SOCKET 完整宽度还原句柄，不能经过 int 截断。
    SOCKET socket = static_cast<SOCKET>(r.fd);
    /// @brief 接受连接使用独立重叠 socket 与稳定地址缓冲区，失败路径须回收临时句柄。
    if (r.kind == kind::accept) {
      if (::setsockopt(node.accepted,
                       SOL_SOCKET,
                       SO_UPDATE_ACCEPT_CONTEXT,
                       reinterpret_cast<const char*>(&socket),
                       sizeof(socket)))
        /// @brief 立即读取 Winsock 错误，后续系统调用不得覆盖本次失败原因。
        return -encode_winsock_error(::WSAGetLastError());
      u_long enabled = 1;
      if (::ioctlsocket(node.accepted, FIONBIO, &enabled))
        /// @brief 立即读取 Winsock 错误，后续系统调用不得覆盖本次失败原因。
        return -encode_winsock_error(::WSAGetLastError());
      if (r.output_address && r.output_address_length) {
        int size = *r.output_address_length;
        if (::getpeername(node.accepted, r.output_address, &size))
          /// @brief 立即读取 Winsock 错误，后续系统调用不得覆盖本次失败原因。
          return -encode_winsock_error(::WSAGetLastError());
        *r.output_address_length = size;
      }
      return static_cast<std::int64_t>(std::exchange(node.accepted, INVALID_SOCKET));
    }
    /// @brief 连接请求按地址族准备本地绑定，再通过当前提供者的 ConnectEx 扩展发起。
    if (r.kind == kind::connect) {
      int type{}, length = sizeof(type);
      if (::getsockopt(socket, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &length))
        /// @brief 立即读取 Winsock 错误，后续系统调用不得覆盖本次失败原因。
        return -encode_winsock_error(::WSAGetLastError());
      if (type == SOCK_STREAM
          && ::setsockopt(socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0))
        /// @brief 立即读取 Winsock 错误，后续系统调用不得覆盖本次失败原因。
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
    /// @brief 成功包直接使用传输量。
    std::int64_t result = packet.dwNumberOfBytesTransferred;
    if (packet.Internal != 0) {
      /// @brief 失败包必须按文件/Winsock 类型解析，不能使用线程残留 LastError。
      DWORD bytes{}, flags{};
      /// @brief 节点只借用域中已经接纳且地址稳定的请求，最终原生完成前域不会回收该请求。
      auto& r = *node.request;
      if (r.native_kind == handle_kind::windows_socket) {
        if (!::WSAGetOverlappedResult(
                static_cast<SOCKET>(r.fd), &node.native.overlapped, &bytes, FALSE, &flags)) {
          /// @brief 立即捕获 Winsock 原生错误，并区分等待完成与同步失败。
          int error = ::WSAGetLastError();
          if (error == WSAEMSGSIZE
              && (r.kind == kind::recv || r.kind == kind::recvfrom || r.kind == kind::recvmsg)) {
            if (r.kind == kind::recvmsg) {
              /// @brief 无地址/control 的 WSARecv 通过 flags 回写。
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
        /// @brief 原生失败后立即保存 Win32 错误，后续清理与输出转换不能覆盖该值。
        DWORD error = ::GetLastError();
        result =
            error == ERROR_HANDLE_EOF && r.kind == kind::read ? 0 : -encode_windows_error(error);
      } else
        result = bytes;
    }
    /// @brief 输出地址/消息头与 accept/connect 上下文只在原完成后更新。
    return finalize(node,
                    result);
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
      /// @brief 只把真实业务完成计入业务统计，独立控制与负载释放另行计数。
      ++statistics_.native_completed;
      recycle(*node);
    }
    return count;
  }

  /// @brief 从内部就绪链移除条目，避免重复交付同一观察结果。
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
          /// @brief 非消费 PEEK，允许假就绪继续保留观察者。
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
        /// @brief 立即读取 Winsock 错误，后续系统调用不得覆盖本次失败原因。
        complete_immediate(
            *node, result < 0 ? -encode_winsock_error(::WSAGetLastError()) : descriptor.revents);
      }
      node = next;
    }
  }

  /** @brief 只在拒绝前或最终结果消费后归还池；接受失败的临时 socket 不泄漏。 */
  void recycle(entry& node) noexcept {
    if (node.accepted != INVALID_SOCKET) {
      /// @brief 释放尚未交付给调用方的临时 socket，不能同时关闭已转出所有权的连接。
      ::closesocket(node.accepted);
      node.accepted = INVALID_SOCKET;
    }
    node.active = false;
    node.request = nullptr;
    node.ready_next = nullptr;
    node.next = free_;
    free_ = &node;
  }

  HANDLE port_{};  ///< 拥有的 IOCP 完成端口句柄，后端销毁时释放。
  mutable std::mutex mutex_;  ///< 串行保护所属对象的共享状态；调度与用户回调应遵循该对象的锁外调用协议。
  mutable std::atomic<bool> wake_pending_{};  ///< 尚未被轮询方消费的控制通知，合并重复唤醒但保留通知责任。
  std::vector<std::unique_ptr<entry>> entries_;  ///< 拥有地址稳定的 IOCP 操作条目，回收前内核不得再引用。
  std::unordered_map<std::uintptr_t, registration> registrations_;  ///< 原生句柄到资源键的长期注册表。
  entry *free_{},  ///< 可复用操作槽或条目的集合，内核解除引用后才允许归还。
      *done_{},  ///< 后端已经完成、等待交付的条目链表头。
      *done_tail_{},  ///< 完成条目链表尾，支持常数时间追加。
      *ready_{};  ///< 待交付的就绪结果或就绪条目链表头。
  std::size_t active_{};  ///< 尚未消费最终完成的已接受后端请求数量。
  detail::backend_statistics statistics_{};  ///< 后端提交、完成及控制事件的诊断统计。
};

}
#endif
