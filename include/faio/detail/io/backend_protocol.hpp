#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace faio::io::detail {
/** @brief 原生注册值不截断 HANDLE/SOCKET；只有 POSIX adapter 转为 int。 */
enum class native_handle_kind : std::uint8_t { posix_descriptor, windows_handle, windows_socket };

struct native_registration {
  std::uintptr_t value{};  ///< 保存完整原生值；不得把 HANDLE 或 Win64 SOCKET 缩窄为 int。
  native_handle_kind kind{native_handle_kind::posix_descriptor};  ///< 拒绝平台种类不匹配的注册。
};

/** @brief 全部后端共用的接受协议；would_queue 尚未保存内核引用。 */
enum class backend_submit_status : std::uint8_t { accepted, would_queue, rejected };

struct backend_submit_result {
  backend_submit_status status{backend_submit_status::rejected};
  int error{};  ///< 正 errno；仅 rejected 使用。
};

/** @brief 原生完成、取消确认与 buffer release 是不同事件，不能混为一次恢复。 */
enum class backend_event_kind : std::uint8_t { readiness, result, cancel_ack, buffer_release };

/** @details poll 只完整填写返回 count 内的记录；默认构造输出槽不清零。
 *          消费者不得读取 count 外的记录；需要空事件时显式使用
 * backend_event{}。
 */
struct backend_event {
  backend_event_kind kind;
  std::uint64_t key;    ///< readiness 为资源代际，其余为操作代际；零为控制通道。
  std::int64_t result;  ///< 结果为字节数/原生结果，失败为负错误码，ACK 不携带业务完成。
  std::uint32_t flags;  ///< 中立 readiness 位或原生完成通知标记。
};

/** @brief 请求描述符指向稳定槽中的拥有型请求，绝不指向 awaiter。 */
struct backend_operation {
  std::uint64_t token{};  ///< 完整 slot/generation，控制 CQE 不可借用 token 高位做 tag。
  void* request{};        ///< 指向域拥有的稳定 typed 请求；accepted
                          ///< 后最后事件前不得移动或销毁。
};

struct backend_flush_result {
  std::size_t submitted{};  ///< 本轮真正交给内核的 SQE 数，允许 partial flush。
  bool pending{};           ///< SQ 或取消控制队列仍有待提交责任，驱动器必须继续推进。
  int error{};              ///< 正错误码，永久失败不能被忽略；可恢复背压由后端保留并重试。
};

/** @brief 可验证的后端工作量快照；原生 CQE 与取消确认分别计数。 */
struct backend_statistics {
  // native_submitted 计入接受并准备的业务 SQE；实际 enter 数由 native_flushed
  // 独立记录。
  std::uint64_t native_submitted{}, native_completed{}, cancel_ack{}, buffer_notifications{};
  std::uint64_t native_flushed{};  ///< io_uring_submit 实際接受的 SQE 总数，包含控制取消。
};

/** @brief 统一后端小函数表；原生 Proactor 与 readiness adapter
 * 共用生命周期协议。
 * @details 原生后端接受 typed request 并产生完成；reactor adapter 只报告
 * readiness， 核心的 syscall attempt
 * 将其转换成同一结果管线。控制方法不可抛异常。
 */
class backend_box {
 public:
  template <class B>
  explicit backend_box(std::unique_ptr<B> object)
      : object_(object.release()), table_(&table_for<B>) {}  // 仅 owning box 接管一次原生后端。

  backend_box(backend_box&& other) noexcept
      : object_(std::exchange(other.object_, nullptr)),
        table_(other.table_) {}  // 移动后旧 box 不析构后端。

  backend_box(const backend_box&) = delete;

  backend_box& operator=(const backend_box&) = delete;

  ~backend_box() {
    if (object_)
      table_->destroy(object_);
  }

  /// @brief 资源注册是控制面操作；失败不接管原生 handle，不产生协程恢复。
  int attach(native_registration handle, std::uint64_t key) noexcept {
    return table_->attach(object_, handle, key);
  }

  int attach(int handle, std::uint64_t key) noexcept {
    return attach({static_cast<std::uintptr_t>(handle), native_handle_kind::posix_descriptor}, key);
  }

  /// @brief 注销 readiness 注册；原生 accepted 请求仍须独立取消并等待最终完成。
  void detach(native_registration handle) noexcept { table_->detach(object_, handle); }

  void detach(int handle) noexcept {
    detach({static_cast<std::uintptr_t>(handle), native_handle_kind::posix_descriptor});
  }

  /// @brief 接受 typed 请求；would_queue/rejected 尚未保存任何内核 payload
  /// 引用。
  backend_submit_result try_submit(backend_operation op) noexcept {
    return table_->submit(object_, op);
  }

  /// @brief 推进有界提交和控制队列，返回实际提交数以及永久驱动错误。
  backend_flush_result flush() noexcept { return table_->flush(object_); }

  /** @brief poll 是否自己推进所有已接受提交及取消责任；未声明的后端保持独立
   * flush。
   * @details 仅消除同一驱动轮次的重复提交调用，不改变 SQE 接受/错误/完成协议。
   */
  bool poll_flushes_submissions() const noexcept { return table_->poll_flushes_submissions; }

  /// @brief 唯一 driver session 批量取得中立事件；nullopt
  /// 无限等待，零非阻塞轮询。
  int poll(std::span<backend_event> out, std::optional<int> timeout) noexcept {
    return table_->poll(object_, out, timeout);
  }

  /** @brief 请求精确代际的取消；控制 ACK 不能代替原业务完成或 buffer release。
   * @param operation 完整 token 和域固定的 typed request；后端不能借用 awaiter
   * 地址。
   */
  void request_cancel(backend_operation operation) noexcept { table_->cancel(object_, operation); }

  /// @brief wake 可跨线程且可合并通知；真实请求与取消责任始终保存在稳定状态中。
  void wake() const noexcept { table_->wake(object_); }

  /// @brief 通知后端停止/唤醒控制面，域仍负责排空全部已经接受的业务责任。
  void begin_shutdown() noexcept { table_->shutdown(object_); }

  /** @brief 永久驱动故障；true 表示后端仍保证排空原生引用并产出最终事件。
   * @details false 表示内核引用边界已不可证明，宿主必须 fail-fast，禁止回收
   * borrowed buffer。
   */
  bool begin_failure(int error) noexcept { return table_->failure(object_, error); }

  /// @brief 只查询后端引用是否排空；域的操作槽、发布者和文件服务另行判断。
  bool quiescent() const noexcept { return table_->quiescent(object_); }

  /// @brief native 仅描述 Proactor 模型；每种 opcode 是否实际实现还要查询
  /// supports。
  bool native() const noexcept { return table_->native; }

  /// @brief 查询具体 opcode 能力；模型为 native 不等于所有平台操作都已实现。
  bool supports(std::uint32_t kind) const noexcept { return table_->supports(object_, kind); }

  /// @brief 返回实际后端静态名称，便于选择验证与可追溯 benchmark。
  const char* name() const noexcept { return table_->name(object_); }

  /// @brief 读取具体后端的工作量快照，不把 readiness 事件冒充原生 CQE 统计。
  backend_statistics statistics() const noexcept { return table_->statistics(object_); }

 private:
  // 小函数表只在每批驱动边界擦除类型；内核/SQE/CQE 绝不保存这个 table 或
  // consumer。
  struct operations {
    int (*attach)(void*, native_registration, std::uint64_t) noexcept;

    void (*detach)(void*, native_registration) noexcept;

    backend_submit_result (*submit)(void*, backend_operation) noexcept;

    backend_flush_result (*flush)(void*) noexcept;

    int (*poll)(void*, std::span<backend_event>, std::optional<int>) noexcept;

    void (*cancel)(void*, backend_operation) noexcept;

    void (*wake)(void*) noexcept;

    void (*shutdown)(void*) noexcept;

    bool (*failure)(void*, int) noexcept;

    bool (*quiescent)(void*) noexcept;

    bool (*supports)(void*, std::uint32_t) noexcept;

    void (*destroy)(void*) noexcept;

    bool native;
    bool poll_flushes_submissions;  ///< 构造时固定的提交能力；第三方后端默认
                                    ///< false。
    const char* (*name)(void*) noexcept;

    backend_statistics (*statistics)(void*) noexcept;
  };

  /** @brief 每种具体后端的静态跳板表，只在用户态调用边界恢复类型。
   * @details requires 在编译期选择 typed handle/cancel
   * 协议，不分配请求包装或创建线程。
   */
  template <class B>
  static inline const operations table_for{
      +[](void* p, native_registration handle, std::uint64_t key) noexcept {
        // typed 注册可保留 Win64 HANDLE/SOCKET 完整值，POSIX
        // 分支才检查缩窄范围。
        if constexpr (requires(B& b) { b.attach(handle, key); })
          return static_cast<B*>(p)->attach(handle, key);  // 原生 Windows 保留 typed 值。
        else {
          // Windows typed backend 永不走 POSIX 缩窄分支。
          if (handle.kind != native_handle_kind::posix_descriptor || handle.value > 0x7fffffffU)
            // 种类或位宽不匹配在接受前明确拒绝，不对错误句柄执行控制调用。
            return -22;
          return static_cast<B*>(p)->attach(static_cast<int>(handle.value), key);
        }
      },
      +[](void* p, native_registration handle) noexcept {
        // 注销遵守与 attach 相同的平台种类，绝不把 Windows 值截断成 int。
        if constexpr (requires(B& b) { b.detach(handle); })
          static_cast<B*>(p)->detach(handle);
        else if (handle.kind == native_handle_kind::posix_descriptor && handle.value <= 0x7fffffffU)
          static_cast<B*>(p)->detach(static_cast<int>(handle.value));
      },
      +[](void* p, backend_operation op) noexcept {
        return static_cast<B*>(p)->try_submit(op);
      },  // 稳定请求转交具体后端。
      +[](void* p) noexcept {
        return static_cast<B*>(p)->flush();
      },  // 一次 flush，不因 partial 递归或忙轮询。
      +[](void* p, std::span<backend_event> out, std::optional<int> ms) noexcept {
        // 仅一次调用具体 poll，完成/就绪解释仍由同一域结果管线负责。
        return static_cast<B*>(p)->poll(out, ms);
      },
      +[](void* p, backend_operation operation) noexcept {
        // 原生取消利用稳定 request 的完成键，兼容后端只需要完整 token。
        if constexpr (requires(B& b) { b.request_cancel(operation); })
          static_cast<B*>(p)->request_cancel(operation);
        else
          // readiness/框架无需原生 payload 地址，仍不能按复用 fd 推断代际。
          static_cast<B*>(p)->request_cancel(operation.token);
      },
      +[](void* p) noexcept { static_cast<B*>(p)->wake(); },
      +[](void* p) noexcept { static_cast<B*>(p)->begin_shutdown(); },
      +[](void* p, int error) noexcept {
        // 只有具体后端能证明永久故障后的内核引用边界，不能由类型擦除层猜测。
        if constexpr (requires(B& b) { b.begin_failure(error); })
          return static_cast<B*>(p)->begin_failure(error);
        else {
          static_cast<B*>(p)->begin_shutdown();
          // 无原生引用的 readiness 可建立排空；未知 native
          // 故障必须拒绝提前回收。
          return !B::native_proactor;
        }
      },
      +[](void* p) noexcept { return static_cast<B*>(p)->quiescent(); },
      +[](void* p, std::uint32_t k) noexcept { return static_cast<B*>(p)->supports(k); },
      // 与创建时 B 精确匹配的 owning 析构，不依赖非虚基类析构。
      +[](void* p) noexcept { delete static_cast<B*>(p); },
      B::native_proactor,  // 恢复具体 owning 析构，避免非虚析构泄漏。
      [] {
        // 可选能力不强迫已有后端改接口；原生模型本身不能证明 poll 已经 flush。
        if constexpr (requires { B::poll_flushes_submissions; })
          return static_cast<bool>(B::poll_flushes_submissions);
        else
          return false;  // 保留独立 flush 故障测试与第三方后端的原有合同。
      }(),
      +[](void* p) noexcept { return static_cast<B*>(p)->name(); },
      +[](void* p) noexcept { return static_cast<B*>(p)->statistics(); }};
  void* object_{};             ///< 唯一原生后端所有者，具体类型只能经过匹配函数表访问。
  const operations* table_{};  ///< 每种后端共用 inline 静态函数表，纯头文件没有 ODR 重复定义。
};
}  // namespace faio::io::detail
