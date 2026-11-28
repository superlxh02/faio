/**
 * @file faio/detail/io/protocol/request.hpp
 * @brief 统一请求与协程注册者。
 */
#pragma once
#include "faio/detail/io/core/fwd.hpp"
#include "faio/detail/io/protocol/completion.hpp"
#include "faio/detail/io/core/types.hpp"
#include "faio/detail/io/protocol/backend.hpp"
#include <chrono>
#include <climits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace faio::io::detail {
inline constexpr std::size_t native_iov_limit =
#ifdef IOV_MAX
    IOV_MAX;
#else
    1024;
#endif
enum class operation_kind : std::uint8_t {
  RECV,
  SEND,
  RECVFROM,
  SENDTO,
  RECVMSG,
  SENDMSG,
  SEND_ZC,
  SENDMSG_ZC,
  CONNECT,
  ACCEPT,
  ACCEPT_NOWAIT,
  READ,
  WRITE,
  READV,
  WRITEV,
  OPEN,
  OPEN2,
  FSYNC,
  CLOSE,
  SHUTDOWN,
  SOCKET,
  READY,
  CANCEL,
  GETSOCKOPT,
  SETSOCKOPT,
  SOCKET_INQ,
  SOCKET_OUTQ,
  STATX,
  MKDIRAT,
  UNLINKAT,
  RENAMEAT,
  LINKAT,
  SYMLINKAT,
  FTRUNCATE,
  UNSUPPORTED
};

/**  @brief 后端无关请求；构造仅保存参数，没有注册或系统调用副作用。 */
struct io_request {
  operation_kind kind{operation_kind::UNSUPPORTED}; /**< 种类标签，用于区分请求、事件或句柄。 */
  bool
      bypass_resource_registration{}; /**< 显式借用请求跳过第二份资源登记；File 外层租约已经固定句柄的生命周期。 */
  bool uncancellable{};    /**< 已接管关闭责任不能被域停机或父任务取消跳过。 */
  bool internal_control{}; /**< 析构时的 close 或半关闭可使用独立、有界且地址稳定的控制记录。 */
  int validation_error{};  /**< 参数错误在 await 时交付，不执行 syscall 或解引用非法描述符。 */
  bool
      observed_would_block{}; /**< 瞬时调用是否已经真实返回暂不可用，接续稳定提交据此避免重复无效尝试。 */
  std::uint64_t
      observed_readiness_generation{}; /**< 实际返回暂不可用时观察到的就绪代际编号，仅编号未变化时可复用该观察。 */
  bool empty_success{};         /**< 仅流接口显式启用空缓冲区成功；数据报保留零长度报文语义。 */
  bool establish_reservation{}; /**< 首次组合 IO 在取得执行权的同一域锁内建立方向租约。 */
  void* reservation{};          /**< 组合 IO 的方向租约标识，地址在持有它的协程帧里稳定。 */
  resource_ptr resource;        /**< 稳定 IO 归属及资源租约。 */
  native_descriptor fd{-1};     /**< 请求使用的完整宽度原生句柄，不能将 Win64 SOCKET 截断为 int。 */
#if defined(_WIN32)
  native_handle_kind native_kind{native_handle_kind::WINDOWS_SOCKET};
  bool windows_implicit_cursor{};
#endif
  std::uint64_t native_completion_key{}; /**< 仅由原生后端锁保护，取消时无需重复映射 token。 */
  void* buffer{};                   /**< 读取时借用的可写字节区，操作安全完成前由调用方保持有效。 */
  const void* const_buffer{};       /**< 发送或写入时借用的只读负载，安全完成前保持有效。 */
  std::size_t length{};             /**< 本次请求的缓冲区长度，单位为字节。 */
  std::uint64_t offset{UINT64_MAX}; /**< UINT64_MAX 表示流式读写，其他值表示按指定偏移读写。 */
  int flags{}, argument{}, argument2{}, argument3{}; /**< 由具体操作解释的第三个原生整数参数。 */
  sockaddr_storage address{};         /**< 拥有的本地或对端 socket 地址，提交期间不借用临时对象。 */
  socklen_t address_length{};         /**< 原生 socket 地址的有效字节长度。 */
  sockaddr* output_address{};         /**< 接收或接受操作写回对端地址的借用存储。 */
  socklen_t* output_address_length{}; /**< 对端地址输出容量及实际长度的借用指针。 */
  msghdr message{};                   /**< 稳定保存的消息描述及缓冲区引用，安全完成前不得迁移。 */
  iovec scalar_vector{};      /**< 原生 recvfrom/sendto 的自有单元素描述符，地址随稳定槽固定。 */
  msghdr* output_message{};   /**< 接收消息时借用的输出描述对象，安全完成前由调用方保持有效。 */
  std::vector<iovec> vectors; /**< iovec 数组由请求拥有；payload 遵守借用契约。 */
  std::uint64_t extension_flags{}, extension_mode{},
      extension_resolve{}; /**< 原生路径解析扩展的约束标志。 */
  std::string path;        /**< 请求拥有的路径字符串，不借用 string_view::data()。 */
  std::string path2;       /**< rename/link/symlink 的第二条路径同样持有到最终 CQE。 */
  std::optional<std::chrono::steady_clock::time_point>
      deadline; /**< 本次操作可选的单调时钟绝对截止时间。 */
};

/**
 * @brief RECV/SEND 等待者独立保存的紧凑参数；完整稳定请求只在需要提交时构造。
 * @details 每个字段都具有确定初始值，不借用原包装对象，不使用未激活 union。
 *          仅描述 RECV/SEND；地址、消息头、路径、iovec 以及文件 offset 不属于
 *          这两个 opcode 的输入。SEND_ZC 和其他请求继续保存原 io_request。
 */
struct scalar_io_request {
  operation_kind kind{operation_kind::UNSUPPORTED}; /**< 构造函数明确设为 recv/send。 */
  bool
      bypass_resource_registration{}; /**< 显式借用请求跳过第二份资源登记；File 外层租约已经固定句柄的生命周期。 */
  bool uncancellable{};        /**< 已接管关闭责任不能被域停机或父任务取消跳过。 */
  bool internal_control{};     /**< 稳定提交时仍保留控制请求的容量策略。 */
  int validation_error{};      /**< 参数拒绝不执行 syscall，也不建立方向租约。 */
  bool observed_would_block{}; /**< 记录 readiness 即时调用已返回 EAGAIN。 */
  std::uint64_t
      observed_readiness_generation{}; /**< 实际返回暂不可用时观察到的就绪代际编号，仅编号未变化时可复用该观察。 */
  bool empty_success{};         /**< 仅流接口显式启用空缓冲区成功；数据报保留零长度报文语义。 */
  bool establish_reservation{}; /**< 首次组合发送在同一域锁内取得方向租约。 */
  void* reservation{};          /**< 组合帧内稳定身份；短写期间保持原方向排他。 */
  resource_ptr resource;        /**< 调用时捕获强租约，直到即时操作结束或移动给稳定槽。 */
  native_descriptor fd{-1};     /**< 原生完整位宽，typed handle 在执行权锁内重新读取。 */
  void* buffer{};               /**< RECV 的借用输出，覆盖真实完成以前的全部内核访问。 */
  const void* const_buffer{};   /**< SEND 的借用输入，保持原数据区生存期契约。 */
  std::size_t length{};         /**< 字节数；不截断、不把短写当作全部完成。 */
  int flags{};                  /**< 原样传给 RECV/SEND，包括 MSG_PEEK/MSG_DONTWAIT。 */
  std::optional<std::chrono::steady_clock::time_point> deadline; /**< 原截止时间。 */

  /**
   * @brief 挂起提交前生成一个独立保存参数的完整请求，再交给原稳定槽/SQE/CQE 状态机。
   * @return 已初始化全部字段的完整请求；非 scalar 输入保持 io_request 默认值。
   * @details resource 只移动，不复制引用计数；这里不执行 IO、登记或保存
   * consumer。 不保存此对象的地址；后端只引用随后 prepare_submit 建立的稳定槽。
   */
  io_request into_request() && noexcept {
    io_request full;
    full.kind = kind;
    full.bypass_resource_registration = bypass_resource_registration;
    full.uncancellable = uncancellable;
    full.internal_control = internal_control;
    full.validation_error = validation_error;
    full.observed_would_block = observed_would_block;
    full.observed_readiness_generation = observed_readiness_generation;
    full.empty_success = empty_success;
    full.establish_reservation = establish_reservation;
    full.reservation = reservation;
    full.resource = std::move(resource);
    full.fd = fd;
    full.buffer = buffer;
    full.const_buffer = const_buffer;
    full.length = length;
    full.flags = flags;
    full.deadline = deadline;
    return full;
  }
};

/** @brief 构造原生 I/O 请求。 */
inline io_request make_request(resource_ptr resource, operation_kind kind) {
  io_request r;
  r.kind = kind;
  r.fd = resource ? -1 : -1;
  r.resource = std::move(resource);
  return r;
}

}  // namespace faio::io::detail
