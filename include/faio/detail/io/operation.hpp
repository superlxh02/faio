#pragma once
#include "faio/detail/common/move_only_function.hpp"
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <sys/uio.h>
#include <utility>
#include <vector>
namespace faio::io {
/** @brief 就绪观察方向；观察者不占用实际读写执行权。 */
enum class Interest : std::uint32_t {
  none = 0,
  readable = 1,
  writable = 2,
  read_write = 3
};
inline constexpr Interest operator|(Interest a, Interest b) noexcept {
  return static_cast<Interest>(static_cast<std::uint32_t>(a) |
                               static_cast<std::uint32_t>(b));
}
/** @brief 就绪快照；允许假就绪，真实 syscall 的 EAGAIN 才清除缓存。 */
struct Ready {
  std::uint32_t flags{};
  std::uint64_t
      generation{}; ///< 本次完成观察到的资源代际，供 readiness guard 条件清除。
  bool is_readable() const noexcept { return flags & 1; }
  bool is_writable() const noexcept { return flags & 2; }
  bool is_error() const noexcept { return flags & 4; }
  /// @brief 读方向已收到 EOF 提示；真实 read 仍可能先返回尚未消费的数据。
  bool is_read_closed() const noexcept { return flags & 8; }
  /// @brief 写方向已关闭；本地写半关闭不表示仍可接收响应的读方向关闭。
  bool is_write_closed() const noexcept { return flags & 16; }
};
enum class ShutdownBehavior {
  Read = SHUT_RD,
  Write = SHUT_WR,
  Both = SHUT_RDWR
};
enum class cancel_reason : std::uint8_t { user, deadline, close, shutdown };
enum class shutdown_policy : std::uint8_t { drain, cancel_all };
/** @brief 槽位加代际 token；后端从不持有 awaiter 地址。 */
struct operation_token {
  std::uint64_t value{};
  friend bool operator==(operation_token, operation_token) = default;
};
struct drive_budget {
  std::size_t max_events{256}, max_completions{256};
};
struct drive_result {
  std::size_t completions{};
  bool progressed{}, more_work{};
  int fatal_error{};
};
namespace detail {
class io_domain;
struct resource_state;
using resource_ptr = std::shared_ptr<resource_state>;
/// @brief 在复制描述符数组之前拒绝不能执行的过量 iovec 请求。
inline constexpr std::size_t native_iov_limit =
#ifdef IOV_MAX
    IOV_MAX;
#else
    1024;
#endif
enum class operation_kind : std::uint8_t {
  recv,
  send,
  recvfrom,
  sendto,
  recvmsg,
  sendmsg,
  send_zc,
  sendmsg_zc,
  connect,
  accept,
  accept_nowait,
  read,
  write,
  readv,
  writev,
  open,
  open2,
  fsync,
  close,
  shutdown,
  socket,
  ready,
  cancel,
  getsockopt,
  setsockopt,
  socket_inq,
  socket_outq,
  statx,
  mkdirat,
  unlinkat,
  renameat,
  linkat,
  symlinkat,
  ftruncate,
  unsupported
};
/** @brief 后端无关请求；构造仅保存参数，没有注册或系统调用副作用。 */
struct io_request {
  operation_kind kind{operation_kind::unsupported};
  bool bypass_resource_registration{}; ///< File 外层持有 lease；原生 raw fd
                                       ///< 请求不注册第二个拥有者。
  bool uncancellable{};        ///< 已接管关闭责任不能被停机/父任务取消跳过。
  bool internal_control{};     ///< 析构close/半关闭可使用独立有界稳定控制记录。
  int validation_error{};      ///< 参数错误在 await 时交付，不执行 syscall
                               ///< 或解引用非法描述符。
  bool observed_would_block{}; ///< 瞬时 try_io 已遇
                               ///< EAGAIN，接续提交可避免重复无效 syscall。
  std::uint64_t observed_readiness_generation{}; ///< 只有事件代际未变才能复用
                                                 ///< would-block 观察。
  bool empty_success{};         ///< 仅流接口显式启用空缓冲区成功；UDP
                                ///< 未启用，保留零长报文语义。
  bool establish_reservation{}; ///< 首次组合 IO
                                ///< 在取得执行权的同一域锁内建立方向租约。
  void *reservation{}; ///< 组合 IO 的方向租约标识，地址在持有它的协程帧里稳定。
  resource_ptr resource; ///< 稳定 IO 归属及资源租约。
  int fd{-1};
  std::uint64_t
      native_completion_key{}; ///< 仅原生backend锁保护，取消无需重复token映射。
  void *buffer{};
  const void *const_buffer{};
  std::size_t length{};
  std::uint64_t offset{
      UINT64_MAX}; ///< UINT64_MAX 为流式读写，否则为 positional IO。
  int flags{}, argument{}, argument2{}, argument3{};
  sockaddr_storage address{};
  socklen_t address_length{};
  sockaddr *output_address{};
  socklen_t *output_address_length{};
  msghdr message{};
  iovec scalar_vector{}; ///< 原生 recvfrom/sendto
                         ///< 的拥有型单元素描述符，地址随稳定槽固定。
  msghdr *output_message{};
  std::vector<iovec> vectors; ///< iovec 数组由请求拥有；payload 遵守借用契约。
  std::uint64_t extension_flags{}, extension_mode{},
      extension_resolve{}; ///< Linux openat2 拥有型可选参数。
  std::string path;        ///< 路径拥有字符串，不能借用 string_view::data()。
  std::string path2; ///< rename/link/symlink 的第二路径同样拥有到最终 CQE。
  std::optional<std::chrono::steady_clock::time_point> deadline;
};
/**
 * @brief RECV/SEND 等待者的紧凑拥有参数；完整稳定请求只在需要提交时构造。
 * @details 每个字段都具有确定初始值，不借用原包装对象，不使用未激活 union。
 *          仅描述 RECV/SEND；地址、消息头、路径、iovec 以及文件 offset 不属于
 *          这两个 opcode 的输入。SEND_ZC 和其他请求继续保存原 io_request。
 */
struct scalar_io_request {
  operation_kind kind{
      operation_kind::unsupported};    ///< 构造函数明确设为 recv/send。
  bool bypass_resource_registration{}; ///< raw fd
                                       ///< 是否跳过资源登记，保持桥的原策略。
  bool uncancellable{};        ///< 稳定提交时仍保留不可取消策略。
  bool internal_control{};     ///< 稳定提交时仍保留控制请求的容量策略。
  int validation_error{};      ///< 参数拒绝不执行 syscall，也不建立方向租约。
  bool observed_would_block{}; ///< readiness 瞬时 EAGAIN 的已执行观察。
  std::uint64_t
      observed_readiness_generation{}; ///< 只复用同一 readiness 代际。
  bool empty_success{}; ///< 流空缓冲区显式成功；UDP 默认仍真实收发零长报文。
  bool establish_reservation{}; ///< 首次组合发送在同一域锁内取得方向租约。
  void *reservation{};          ///< 组合帧内稳定身份；短写期间保持原方向排他。
  resource_ptr resource; ///< 调用时捕获强租约，直到瞬时结束或移动给稳定槽。
  int fd{-1};            ///< raw fd 输入；typed fd 在执行权锁内重新读取。
  void *buffer{};        ///< RECV 的借用输出，覆盖真实完成以前的全部内核访问。
  const void *const_buffer{}; ///< SEND 的借用输入，保持原数据区生存期契约。
  std::size_t length{};       ///< 字节数；不截断、不把短写当作全部完成。
  int flags{}; ///< 原样传给 RECV/SEND，包括 MSG_PEEK/MSG_DONTWAIT。
  std::optional<std::chrono::steady_clock::time_point>
      deadline; ///< 原截止时间。

  /**
   * @brief 挂起提交前生成一个完整拥有请求，再交给原稳定槽/SQE/CQE 状态机。
   * @return 已初始化全部字段的完整请求；非 scalar 输入保持 io_request 默认值。
   * @details resource 只移动，不复制引用计数；这里不执行 IO、登记或保存
   * consumer。 不保存此对象的地址；后端只引用随后 prepare_submit 建立的稳定槽。
   */
  io_request into_request() && noexcept {
    io_request full;  // 冷字段仍完整初始化，原生后端不读取不确定表示。
    full.kind = kind; // 进入原 opcode 能力检查，不用 syscall 冒充原生完成。
    full.bypass_resource_registration = bypass_resource_registration;
    full.uncancellable = uncancellable;
    full.internal_control = internal_control;
    full.validation_error = validation_error;
    full.observed_would_block = observed_would_block;
    full.observed_readiness_generation = observed_readiness_generation;
    full.empty_success = empty_success;
    full.establish_reservation = establish_reservation;
    full.reservation = reservation;
    full.resource =
        std::move(resource); // 稳定请求接管调用时捕获的同一个控制块。
    full.fd = fd;
    full.buffer = buffer;
    full.const_buffer = const_buffer;
    full.length = length;
    full.flags = flags;
    full.deadline = deadline;
    return full; // 同类型局部返回可 NRVO；即使移动也保持完整拥有语义。
  }
};

/** @brief 消费者函数表；核心不知道 coroutine_handle 或 scheduler。 */
struct completion_target {
  void *consumer{};
  void (*publish)(void *, std::int64_t, std::uint64_t) noexcept {};
};
/** @brief 地址稳定的池化操作；全部普通字段由 domain 锁串行保护。 */
struct operation_state {
  io_request request;
  completion_target target;
  ::faio::move_only_function<void(int)>
      control_completion; ///< 孤立close的拥有型CQE回调。
  operation_token token{};
  std::uint32_t slot{}, generation{};
  std::int64_t result{};
  std::uint64_t transferred{};
  int cancellation{};
  bool counts_active{}, allocated{}, accepted{}, terminal{}, running_file{},
      connecting{}, uses_reader{}, uses_writer{}, observer{},
      queued_completion{};
  bool native_inflight{}, native_pending{}, native_cancel_requested{},
      native_waiting_release{}, native_close{};
  operation_state *next{}; ///< 内嵌完成节点；发布完成时无需分配内存。
  operation_state
      *completed_previous{}; ///< 双向完成链允许 O(1) 领取当前即时完成。
  operation_state *observer_next{};
};
inline io_request make_request(resource_ptr resource, operation_kind kind) {
  io_request r;
  r.kind = kind;
  r.fd = resource ? -1 : -1;
  r.resource = std::move(resource);
  return r;
}
} // namespace detail
} // namespace faio::io
