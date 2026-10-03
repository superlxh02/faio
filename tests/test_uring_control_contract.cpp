/**
 * @file test_uring_control_contract.cpp
 * @brief 使用真实 uring_backend 验证控制提交背压；不伪造业务 CQE 或恢复协程。
 * @details 本文件是专用可执行文件的唯一 TU，只有该目标定义 FAIO_URING_CONTROL_TESTS。
 *          hook 仅替换指定的一次 submit 返回值；其余 SQ/CQ/entry/TIMEOUT 全部来自原生内核。
 *          wait hook 记录不应发生的 GETEVENTS 并有限返回，让错误版本也不会挂住测试进程。
 */
#include "faio/faio.hpp"
#include <array>
#include <atomic>
#include <coroutine>
#include <semaphore>
#include <thread>
#include <cerrno>
#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <system_error>

#if defined(__linux__) && defined(FAIO_HAS_IO_URING) && FAIO_HAS_IO_URING
#include "faio/detail/io/backends/uring/backend.hpp"
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace faio::io::detail {
/** @brief 编译期 seam 的唯一访问者；真实生产构建没有 friend、hook 或额外字段。 */
struct uring_control_test_access {
  using point = uring_backend::control_test_point;

  static void wake_hooks(uring_backend& backend,
                         void (*step)(point, void*) noexcept,
                         ssize_t (*write)(int, const void*, std::size_t, void*) noexcept) noexcept {
    backend.control_test_step_ = step;
    backend.control_test_write_ = write;
  }

  static bool notice(const uring_backend& backend) noexcept {
    return backend.wake_pending_.load(std::memory_order_seq_cst);
  }

  static bool has_extended_wait(const uring_backend& backend) noexcept {
    return (backend.ring_.features & IORING_FEAT_EXT_ARG) != 0;
  }

  static int finite_wait(uring_backend& backend, int milliseconds) noexcept {
    __kernel_timespec duration{milliseconds / 1000,
                               static_cast<long long>(milliseconds % 1000) * 1000000};
    io_uring_getevents_arg argument{};
    argument.ts = reinterpret_cast<std::uintptr_t>(&duration);
    return ::io_uring_enter2(backend.ring_.ring_fd,
                             0,
                             1,
                             IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG,
                             reinterpret_cast<sigset_t*>(&argument),
                             sizeof(argument));
  }

  static ssize_t write_count(uring_backend& backend, std::uint64_t value) noexcept {
    return ::write(backend.wake_fd_, &value, sizeof(value));
  }

  static void hooks(uring_backend& backend,
                    int (*submit)(io_uring*, void*) noexcept,
                    int (*wait)(void*) noexcept,
                    void* context) noexcept {
    backend.control_test_submit_ = submit;
    backend.control_test_wait_ = wait;
    backend.control_test_step_ = nullptr;
    backend.control_test_write_ = nullptr;
    backend.control_test_context_ = context;
  }

  static unsigned pending(uring_backend& backend) noexcept {
    return ::io_uring_sq_ready(&backend.ring_);
  }

  static unsigned space(uring_backend& backend) noexcept {
    return ::io_uring_sq_space_left(&backend.ring_);
  }

  static std::size_t entries(const uring_backend& backend) noexcept {
    return backend.entries_.size();
  }

  static std::size_t pending_cancels(const uring_backend& backend) noexcept {
    return backend.pending_cancels_.size();
  }

  static std::uint64_t timer(const uring_backend& backend) noexcept { return backend.timer_key_; }

  static const void* timer_payload(const uring_backend& backend) noexcept {
    const auto found = backend.entries_.find(backend.timer_key_);
    return found == backend.entries_.end() ? nullptr : &found->second.timeout;
  }

  static int prepare_timer(uring_backend& backend, int milliseconds) noexcept {
    return backend.arm_wait_timer(milliseconds);
  }

  static void cancel_timer(uring_backend& backend) noexcept { backend.cancel_wait_timer(); }

  static int ring_fd(const uring_backend& backend) noexcept { return backend.ring_.ring_fd; }

  static bool append_nop(uring_backend& backend) noexcept {
    auto* sqe = ::io_uring_get_sqe(&backend.ring_);
    if (!sqe)
      return false;
    ::io_uring_prep_nop(sqe);
    ::io_uring_sqe_set_data64(sqe, 0);  // 原生无 payload 的控制 NOP，不冒充业务完成。
    return true;
  }
};
}  // namespace faio::io::detail

namespace {
using namespace faio::io;
using namespace faio::io::detail;
using control_access = uring_control_test_access;

/** @brief 精确提交次数脚本；只有指定一次返回背压，其他调用保持真实 liburing 提交。 */
struct pressure_script {
  unsigned submits{}, waits{}, fail_at{1};
  int error{EAGAIN};

  static int submit(io_uring* ring, void* context) noexcept {
    auto& script = *static_cast<pressure_script*>(context);
    if (++script.submits == script.fail_at)
      return -script.error;  // 既不发布 SQ tail，也不生成 CQE；原 SQ 槽责任全部保留。
    return ::io_uring_submit(ring);
  }

  static int wait(void* context) noexcept {
    ++static_cast<pressure_script*>(context)->waits;
    return -EAGAIN;  // 错误进入等待时只记录，绝不无限阻塞或制造业务结果。
  }
};

/** @brief 请求/缓冲区/描述符由 fixture 拥有；任何 ASSERT 早退仍先排空真实内核引用。 */
class UringControlContract : public ::testing::Test {
 protected:
  void SetUp() override {
    try {
      backend_ = std::make_unique<uring_backend>(8, false);  // 强制原生 TIMEOUT 兼容分支。
    } catch (const std::system_error& error) {
      if (error.code().value() == EPERM || error.code().value() == ENOSYS
          || error.code().value() == EOPNOTSUPP)
        GTEST_SKIP() << "宿主不允许原生 io_uring: " << error.what();
      FAIL() << error.what();
    }
    ASSERT_EQ(
        ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, descriptors_.data()),
        0);
  }

  void TearDown() override {
    if (backend_) {
      control_access::hooks(*backend_, nullptr, nullptr, nullptr);  // 清除背压与 wait 替身。
      if (request_.native_completion_key)
        backend_->request_cancel({1, &request_});  // fixture 缓冲区一直保留到真实最后 CQE。
      control_access::cancel_timer(*backend_);     // TIMEOUT 与其取消 ACK 均按原管线排空。
      std::array<backend_event, 16> events;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (!backend_->quiescent() && std::chrono::steady_clock::now() < deadline) {
        (void)backend_->poll(events, 0);  // 每轮有界提交/消费，不使用无限等待或测试假完成。
        if (!backend_->quiescent()) {
          pollfd ring{control_access::ring_fd(*backend_), POLLIN, 0};
          (void)::poll(&ring, 1, 5);  // 仅有限观察真实 CQ；这不是生产业务 readiness 路径。
        }
      }
      if (!backend_->quiescent()) {
        ADD_FAILURE() << "真实内核引用未排空，不能销毁 fixture payload";
        std::terminate();  // 遵循 native lease 的 fail-fast 合同，禁止释放仍被内核借用的对象。
      }
      backend_.reset();  // 最后 CQE 之后先回收 ring，再关闭测试描述符/销毁拥有型请求。
    }
    for (const auto descriptor : descriptors_)
      if (descriptor >= 0)
        ::close(descriptor);
  }

  void install_pressure() noexcept {
    control_access::hooks(*backend_, pressure_script::submit, pressure_script::wait, &script_);
  }

  backend_submit_result submit_request(operation_kind kind) noexcept {
    request_.kind = kind;
    request_.fd = descriptors_[0];
    request_.buffer = &payload_;
    request_.const_buffer = &payload_;  // 原生 SEND 使用只读输入指针，RECV 使用 buffer 输出指针。
    request_.length = 1;
    return backend_->try_submit({1, &request_});
  }

  std::unique_ptr<uring_backend> backend_;
  pressure_script script_;
  std::array<int, 2> descriptors_{-1, -1};
  io_request request_;  // 地址在整个 fixture 中稳定，TearDown 之前不会移动或销毁。
  char payload_{'p'};
};

class UringTransientSubmitPressure : public UringControlContract,
                                     public ::testing::WithParamInterface<int> {};

TEST_P(UringTransientSubmitPressure, PendingControlNeverEntersInfiniteGetevents) {
  script_.error = GetParam();
  install_pressure();
  std::array<backend_event, 2> output{};
  EXPECT_EQ(backend_->poll(output, std::nullopt), 0);
  EXPECT_EQ(script_.submits, 1u);  // 只注入一次真正 submit 边界，不建立另一个假 backend。
  EXPECT_EQ(script_.waits, 0u);
  EXPECT_GT(control_access::pending(*backend_), 0u);
  EXPECT_EQ(control_access::entries(*backend_), 0u);
  EXPECT_EQ(control_access::timer(*backend_), 0u);
  EXPECT_EQ(backend_->statistics().native_completed, 0u);
  EXPECT_EQ(output[0].key, 0u);  // 控制提前返回没有写业务结果槽。
}

INSTANTIATE_TEST_SUITE_P(RecoverableErrors,
                         UringTransientSubmitPressure,
                         ::testing::Values(EAGAIN, EINTR, ENOMEM));

TEST_F(UringControlContract, FullSqWithPendingCancelCannotRelyOnUnsubmittedWake) {
  ASSERT_EQ(submit_request(operation_kind::recv).status, backend_submit_status::accepted);
  const auto space = control_access::space(*backend_);
  for (unsigned index = 0; index < space; ++index)
    ASSERT_TRUE(
        control_access::append_nop(*backend_));  // 只占满现有有界容量，无 payload 额外借用。
  ASSERT_FALSE(control_access::append_nop(*backend_));
  backend_->request_cancel({1, &request_});  // 没有 SQ 空位，取消责任留在原 pending 队列。
  ASSERT_EQ(control_access::pending_cancels(*backend_), 1u);
  install_pressure();
  std::array<backend_event, 2> output{};
  EXPECT_EQ(backend_->poll(output, std::nullopt), 0);
  EXPECT_EQ(script_.waits, 0u);
  EXPECT_EQ(control_access::pending_cancels(*backend_), 1u);
  EXPECT_NE(request_.native_completion_key, 0u);  // 原请求 lease 与完整原生键均未提前解除。
  EXPECT_EQ(backend_->statistics().native_completed, 0u);
}

TEST_F(UringControlContract, PendingSubmissionStillHarvestsAlreadyCompletedBusiness) {
  ASSERT_EQ(submit_request(operation_kind::send).status, backend_submit_status::accepted);
  ASSERT_EQ(backend_->flush().error, 0);  // 真正原生 SEND；不通过 seam 注入结果或 CQE。
  pollfd ring{control_access::ring_fd(*backend_), POLLIN, 0};
  ASSERT_GT(::poll(&ring, 1, 1000), 0);  // 有限等待真实 CQ 就绪，fixture 保持 buffer 活着。
  ASSERT_TRUE(control_access::append_nop(*backend_));
  install_pressure();
  std::array<backend_event, 2> output{};
  EXPECT_EQ(backend_->poll(output, std::nullopt), 1);
  EXPECT_EQ(script_.waits, 0u);
  EXPECT_GT(control_access::pending(*backend_), 0u);  // 未提交 NOP 保留，不以消费 SEND 伪装提交。
  EXPECT_EQ(output[0].kind, backend_event_kind::result);
  EXPECT_EQ(output[0].key, 1u);
  EXPECT_EQ(output[0].result, 1);
  EXPECT_EQ(request_.native_completion_key, 0u);  // 只有真实 SEND CQE 才解除该完成记录。
  EXPECT_EQ(backend_->statistics().native_completed, 1u);
}

TEST_F(UringControlContract, FirstSubmitPressureDoesNotCreateAnUnusableTimeout) {
  install_pressure();
  std::array<backend_event, 2> output{};
  EXPECT_EQ(backend_->poll(output, 10000), 0);
  EXPECT_EQ(script_.waits, 0u);
  EXPECT_EQ(script_.submits, 1u);
  EXPECT_EQ(control_access::timer(*backend_), 0u);
  EXPECT_EQ(control_access::entries(*backend_), 0u);
}

TEST_F(UringControlContract, TimeoutSubmitPressurePreservesSingleStableEntryAcrossRetry) {
  script_.fail_at = 2;  // 第一次提交原生 wake 成功，第二次仅 TIMEOUT SQE 被确定性背压。
  install_pressure();
  std::array<backend_event, 2> output{};
  EXPECT_EQ(backend_->poll(output, 10000), 0);
  EXPECT_EQ(script_.waits, 0u);
  ASSERT_EQ(script_.submits, 2u);
  const auto timer = control_access::timer(*backend_);
  ASSERT_NE(timer, 0u);
  ASSERT_EQ(control_access::entries(*backend_), 1u);
  const auto* payload = control_access::timer_payload(*backend_);
  ASSERT_NE(payload, nullptr);
  EXPECT_GT(control_access::pending(*backend_), 0u);
  script_.fail_at = 3;  // 下一轮的独立一次提交仍背压；不能再创建第二个 timer。
  EXPECT_EQ(backend_->poll(output, 5000), 0);
  EXPECT_EQ(script_.waits, 0u);
  EXPECT_EQ(control_access::timer(*backend_), timer);
  EXPECT_EQ(control_access::entries(*backend_), 1u);
  EXPECT_EQ(control_access::timer_payload(*backend_),
            payload);   // 内核将借用的 timespec 地址始终不变。
  script_.fail_at = 0;  // 真正重试同一个原生 TIMEOUT，然后 fixture 原生取消并排空。
  EXPECT_EQ(backend_->poll(output, 0), 0);
  EXPECT_EQ(control_access::timer(*backend_), timer);
  EXPECT_EQ(control_access::timer_payload(*backend_), payload);
  EXPECT_EQ(backend_->statistics().native_completed, 0u);
}

TEST_F(UringControlContract, ExistingAcceptedTimeoutKeepsIdentityDuringUnrelatedSqPressure) {
  ASSERT_EQ(control_access::prepare_timer(*backend_, 10000), 0);
  const auto timer = control_access::timer(*backend_);
  ASSERT_NE(timer, 0u);
  ASSERT_EQ(backend_->flush().error, 0);  // 旧 timer 确实已交内核；不是只有用户态 prep。
  ASSERT_TRUE(control_access::append_nop(*backend_));
  install_pressure();
  std::array<backend_event, 2> output{};
  EXPECT_EQ(backend_->poll(output, 25), 0);
  EXPECT_EQ(script_.waits, 0u);
  EXPECT_EQ(control_access::timer(*backend_), timer);
  EXPECT_EQ(control_access::entries(*backend_), 1u);
  EXPECT_EQ(backend_->statistics().native_completed, 0u);
}

/** @brief 所有暂停均有一秒边界；父线程失败仍可释放/回收子线程，不使用随机 sleep 碰窗口。 */
struct control_gate {
  std::binary_semaphore arrived{0}, released{0};
  std::atomic_bool expired{false};

  void stop() noexcept {
    arrived.release();
    if (!released.try_acquire_for(std::chrono::seconds(1)))
      expired.store(true, std::memory_order_relaxed);
  }

  bool await() noexcept { return arrived.try_acquire_for(std::chrono::seconds(1)); }

  void open() noexcept { released.release(); }
};

/** @brief 计数与暂停只存在专用测试 TU；实际入口/wake/drain 实现来自真实 uring_backend。 */
struct wake_script {
  uring_backend* backend{};
  control_gate gate;
  control_access::point pause_on{control_access::point::poll_after_load};
  bool pause_enabled{}, interrupt_first_write{};
  std::atomic<unsigned> hits{0}, writes{0}, successful_writes{0}, waits{0}, drains{0};
  std::atomic<int> last_write_error{0};
  std::atomic_bool invalid_wait{false};

  static void step(control_access::point point, void* context) noexcept {
    auto& script = *static_cast<wake_script*>(context);
    if (point == control_access::point::harvest_after_drain)
      script.drains.fetch_add(1, std::memory_order_relaxed);
    if (script.pause_enabled && point == script.pause_on
        && script.hits.fetch_add(1, std::memory_order_relaxed) == 0)
      script.gate.stop();  // 只暂停第一个匹配事件；第二个 epoch 可正常继续发布。
  }

  static ssize_t write(int fd, const void* value, std::size_t size, void* context) noexcept {
    auto& script = *static_cast<wake_script*>(context);
    const auto attempt = script.writes.fetch_add(1, std::memory_order_relaxed) + 1;
    if (script.interrupt_first_write && attempt == 1) {
      errno = EINTR;
      script.last_write_error.store(EINTR, std::memory_order_relaxed);
      return -1;  // 注入一次 syscall 中断，不清 latch 或写任何 eventfd 计数。
    }
    const auto result = ::write(fd, value, size);
    if (result >= 0)
      script.successful_writes.fetch_add(1, std::memory_order_relaxed);
    else
      script.last_write_error.store(errno, std::memory_order_relaxed);
    return result;  // EAGAIN 使用真实饱和 eventfd，不注入假的可读状态。
  }

  static int wait(void* context) noexcept {
    auto& script = *static_cast<wake_script*>(context);
    script.waits.fetch_add(1, std::memory_order_relaxed);
    const int result = control_access::finite_wait(*script.backend, 500);
    if (result < 0 && result != -EINTR)
      script.invalid_wait.store(true, std::memory_order_relaxed);
    return result;  // 真正 GETEVENTS + 500ms EXT_ARG；不生成假 CQE，错误版本也有有限上界。
  }
};

enum class wake_window {
  true_load_before_clear,
  false_load_before_publish,
  clear_before_publish,
  before_drain,
  after_drain,
  producer_after_exchange,
  producer_before_write
};

struct wake_window_case {
  wake_window window;
  bool nonblocking;
};

class UringWakeContract : public UringControlContract {
 protected:
  void SetUp() override {
    UringControlContract::SetUp();
    if (::testing::Test::HasFailure() || ::testing::Test::IsSkipped())
      return;
    // 只是测试等待保险需要 EXT_ARG；生产旧内核 TIMEOUT 已由 P 的实际控制测试覆盖。
    if (!control_access::has_extended_wait(*backend_))
      GTEST_SKIP() << "确定性原生窗口测试的有限 GETEVENTS 保险需要 EXT_ARG";
    wake_.backend = backend_.get();
    control_access::hooks(*backend_, nullptr, wake_script::wait, &wake_);
    control_access::wake_hooks(*backend_, wake_script::step, wake_script::write);
  }

  void arm_control() {
    ASSERT_EQ(backend_->flush().error, 0);
    ASSERT_EQ(control_access::pending(*backend_), 0u);
  }

  void old_control_ready() {
    arm_control();
    backend_->wake();
    backend_->wake();  // 同一 epoch 两个真实通知应只调用一次原生 write。
    ASSERT_EQ(wake_.writes.load(), 1u);
    ASSERT_GE(control_access::finite_wait(*backend_, 500),
              0);  // 有界取得 CQ 就绪，不消费/清 latch。
  }

  void verify_control_return() {
    EXPECT_FALSE(wake_.gate.expired.load());
    EXPECT_FALSE(wake_.invalid_wait.load());
    EXPECT_EQ(backend_->statistics().native_completed, 0u);  // 控制不会冒充业务完成。
    EXPECT_EQ(control_access::entries(*backend_), 0u);       // 未创建业务请求或旧内核 timer。
  }

  wake_script wake_;
};

class UringWakeWindows : public UringWakeContract,
                         public ::testing::WithParamInterface<wake_window_case> {};

TEST_P(UringWakeWindows, ActualPollOwnsNoticeAcrossPreciseInterleaving) {
  const auto test = GetParam();
  const auto timeout = test.nonblocking ? std::optional<int>{0} : std::nullopt;
  const bool delayed = test.window == wake_window::producer_after_exchange
                       || test.window == wake_window::producer_before_write;
  std::array<backend_event, 2> output{};
  wake_.pause_enabled = true;
  if (delayed) {
    wake_.pause_on = test.window == wake_window::producer_after_exchange
                         ? control_access::point::wake_after_exchange
                         : control_access::point::wake_before_write;
    arm_control();
    std::jthread producer([&] { backend_->wake(); });  // 首位发布者已置 true，但尚未写 eventfd。
    const bool stopped = wake_.gate.await();
    int result = -1;
    if (stopped) {
      result = backend_->poll(output, timeout);  // 入口取得通知，write 尚未发生也不得无限等待。
      EXPECT_FALSE(control_access::notice(*backend_));
      EXPECT_EQ(wake_.waits.load(), 0u);
      EXPECT_EQ(wake_.writes.load(), 0u);
      backend_->wake();  // 新 epoch 的生产者先写，再允许旧生产者晚到；两份写入责任均保留。
    }
    wake_.gate.open();
    producer.join();
    ASSERT_TRUE(stopped);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(wake_.writes.load(), 2u);
    ASSERT_TRUE(control_access::notice(*backend_));
    // 写入责任已经兑现，但 write 返回不保证 CQE 同步生成；有限确认真实 CQ 就绪，不消费 latch/CQ。
    ASSERT_GE(control_access::finite_wait(*backend_, 500), 0);
    ASSERT_TRUE(control_access::notice(*backend_));
    EXPECT_EQ(backend_->poll(output, std::nullopt),
              0);  // 新 epoch 仍禁止生产等待，旧延迟写只增控制计数。
    EXPECT_FALSE(control_access::notice(*backend_));
    EXPECT_EQ(wake_.waits.load(), 0u);
    EXPECT_GT(wake_.drains.load(), 0u);
    verify_control_return();
    return;
  }

  switch (test.window) {
    case wake_window::true_load_before_clear:
      wake_.pause_on = control_access::point::poll_after_load;
      break;
    case wake_window::false_load_before_publish:
      wake_.pause_on = control_access::point::poll_after_load;
      break;
    case wake_window::clear_before_publish:
      wake_.pause_on = control_access::point::poll_after_consume;
      break;
    case wake_window::before_drain:
      wake_.pause_on = control_access::point::harvest_before_drain;
      break;
    case wake_window::after_drain:
      wake_.pause_on = control_access::point::harvest_after_drain;
      break;
    default:
      FAIL() << "delayed 已单独覆盖";
  }
  if (test.window == wake_window::false_load_before_publish)
    arm_control();
  else
    old_control_ready();  // 真 CQE 已存在，不靠调度概率碰到 drain 暂停点。
  std::atomic<int> result{-1};
  std::jthread consumer([&] { result.store(backend_->poll(output, timeout)); });
  const bool stopped = wake_.gate.await();
  if (stopped)
    backend_->wake();  // 精确在线性化 clear 以前/以后或者真实 drain 以前/以后发布。
  wake_.gate.open();
  consumer.join();
  ASSERT_TRUE(stopped);
  EXPECT_EQ(result.load(), 0);
  if (test.window == wake_window::false_load_before_publish) {
    ASSERT_TRUE(control_access::notice(*backend_));  // false 入口没有领取本次通知责任。
    if (test.nonblocking)
      EXPECT_EQ(wake_.waits.load(), 0u);  // poll(0) 不为等控制 CQE进入任何生产等待。
    if (wake_.drains.load() == 0) {
      // 本轮零等待/控制推进不保证 CQE 已生成；后续有限观察保证最终 drain，不改本轮语义。
      ASSERT_GE(control_access::finite_wait(*backend_, 500), 0);
      ASSERT_TRUE(control_access::notice(*backend_));  // 有限等待不消费 CQ，也不清 latch。
    }
  }
  if (test.window == wake_window::true_load_before_clear) {
    EXPECT_EQ(wake_.writes.load(), 1u);  // clear 以前的第三个通知合并进当前 noticed。
    EXPECT_FALSE(control_access::notice(*backend_));
    EXPECT_EQ(wake_.waits.load(), 0u);
  } else {
    EXPECT_EQ(wake_.writes.load(), test.window == wake_window::false_load_before_publish ? 1u : 2u);
    ASSERT_TRUE(control_access::notice(*backend_));  // harvest 即使排空新字节，也不得清新 latch。
    const auto waits_before = wake_.waits.load();
    EXPECT_EQ(backend_->poll(output, std::nullopt),
              0);                                 // 下一个阻塞 poll 必须领取 retained notice 返回。
    EXPECT_EQ(wake_.waits.load(), waits_before);  // 空内核计数不能把新 epoch 带进无限等待。
    EXPECT_FALSE(control_access::notice(*backend_));
  }
  // 旧 CQ 场景已先有限确保 CQ 就绪；false-load 场景如有异步生成也已有限确认再实际 harvest。
  EXPECT_GT(wake_.drains.load(), 0u);
  verify_control_return();
}

/** @brief 普通函数生成参数名，避免模板逗号被 GoogleTest 的可变参数宏拆分。 */
std::string wake_window_name(const ::testing::TestParamInfo<wake_window_case>& info) {
  const std::array<const char*, 7> names{"TrueLoadBeforeClear",
                                         "FalseLoadBeforePublish",
                                         "ClearBeforePublish",
                                         "BeforeDrain",
                                         "AfterDrain",
                                         "ProducerAfterExchange",
                                         "ProducerBeforeWrite"};
  return std::string(names[static_cast<std::size_t>(info.param.window)])
         + (info.param.nonblocking ? "Nonblocking" : "Blocking");
}

INSTANTIATE_TEST_SUITE_P(
    ExactWindows,
    UringWakeWindows,
    ::testing::Values(wake_window_case{wake_window::true_load_before_clear, true},
                      wake_window_case{wake_window::true_load_before_clear, false},
                      wake_window_case{wake_window::false_load_before_publish, true},
                      wake_window_case{wake_window::false_load_before_publish, false},
                      wake_window_case{wake_window::clear_before_publish, true},
                      wake_window_case{wake_window::clear_before_publish, false},
                      wake_window_case{wake_window::before_drain, true},
                      wake_window_case{wake_window::before_drain, false},
                      wake_window_case{wake_window::after_drain, true},
                      wake_window_case{wake_window::after_drain, false},
                      wake_window_case{wake_window::producer_after_exchange, true},
                      wake_window_case{wake_window::producer_after_exchange, false},
                      wake_window_case{wake_window::producer_before_write, true},
                      wake_window_case{wake_window::producer_before_write, false}),
    wake_window_name);

TEST_F(UringWakeContract, PublicationAfterFinalHarvestInterruptsActualKernelWait) {
  arm_control();
  wake_.pause_enabled = true;
  wake_.pause_on = control_access::point::poll_before_wait;
  std::array<backend_event, 2> output{};
  std::atomic<int> result{-1};
  std::jthread consumer([&] { result.store(backend_->poll(output, std::nullopt)); });
  const bool stopped = wake_.gate.await();
  if (stopped)
    backend_->wake();  // false load/空 CQ 已经经过；新 write 必须能结束真正的 GETEVENTS。
  wake_.gate.open();
  consumer.join();
  ASSERT_TRUE(stopped);
  EXPECT_EQ(result.load(), 0);
  EXPECT_EQ(wake_.waits.load(), 1u);
  EXPECT_GT(wake_.drains.load(), 0u);
  ASSERT_TRUE(control_access::notice(*backend_));
  EXPECT_EQ(backend_->poll(output, std::nullopt), 0);
  EXPECT_EQ(wake_.waits.load(), 1u);  // 第二个入口取 notice，不能重复进入内核等待。
  verify_control_return();
}

TEST_F(UringWakeContract, InterruptedOnlyWriterRetriesWithoutDroppingEpoch) {
  arm_control();
  wake_.interrupt_first_write = true;
  backend_->wake();
  EXPECT_EQ(wake_.writes.load(), 2u);
  EXPECT_EQ(wake_.successful_writes.load(), 1u);
  std::array<backend_event, 2> output{};
  EXPECT_EQ(backend_->poll(output, std::nullopt), 0);
  EXPECT_EQ(wake_.waits.load(), 0u);
  EXPECT_FALSE(control_access::notice(*backend_));
  verify_control_return();
}

TEST_F(UringWakeContract, SaturatedNativeEventfdIsAlreadyAReadableNotification) {
  arm_control();
  ASSERT_EQ(control_access::write_count(*backend_, UINT64_MAX - 1), 8);
  backend_->wake();  // 真实 syscall 因饱和返回 EAGAIN，已有计数足以产生原生 wake CQE。
  EXPECT_EQ(wake_.writes.load(), 1u);
  EXPECT_EQ(wake_.last_write_error.load(), EAGAIN);
  std::array<backend_event, 2> output{};
  EXPECT_EQ(backend_->poll(output, std::nullopt), 0);
  EXPECT_EQ(wake_.waits.load(), 0u);
  EXPECT_FALSE(control_access::notice(*backend_));
  verify_control_return();
}

TEST_F(UringWakeContract, ConstWakeAndShutdownShareOnePendingControlEpoch) {
  arm_control();
  const auto& constant_backend = *backend_;
  constant_backend.wake();
  constant_backend.wake();
  backend_->begin_shutdown();
  EXPECT_EQ(wake_.writes.load(), 1u);
  std::array<backend_event, 2> output{};
  EXPECT_EQ(backend_->poll(output, std::nullopt), 0);
  EXPECT_EQ(wake_.waits.load(), 0u);
  EXPECT_FALSE(control_access::notice(*backend_));
  verify_control_return();
}

/** @brief 测试拥有的真实挂起帧；调度队列借用句柄，完成重查后由此 owner 回收。 */
struct queued_probe {
  struct promise_type {
    queued_probe get_return_object() noexcept {
      return queued_probe{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    std::suspend_always initial_suspend() noexcept { return {}; }

    std::suspend_always final_suspend() noexcept { return {}; }

    void return_void() noexcept {}

    void unhandled_exception() noexcept { std::terminate(); }
  };

  explicit queued_probe(std::coroutine_handle<promise_type> value) noexcept : handle(value) {}

  queued_probe(const queued_probe&) = delete;

  queued_probe(queued_probe&& other) noexcept : handle(std::exchange(other.handle, {})) {}

  ~queued_probe() {
    if (handle)
      handle.destroy();
  }

  std::coroutine_handle<promise_type> handle;
};

queued_probe make_queued_probe() {
  co_return;
}

TEST_F(UringWakeContract, NonblockingConsumeStillPreventsCurrentThreadQueueSleep) {
  faio::runtime::detail::current_thread_scheduler scheduler;
  auto queued = make_queued_probe();
  scheduler.enqueue(queued.handle);  // 没有 thread binding，使用真实 mutex/remote_pending 队列。
  backend_->wake();
  std::array<backend_event, 2> output{};
  ASSERT_EQ(backend_->poll(output, 0), 0);  // 消费通知，但此处刻意尚未调用外层 next/has_ready。
  ASSERT_FALSE(control_access::notice(*backend_));
  EXPECT_FALSE(
      scheduler.prepare_sleep());  // 同生产者 mutex 的真实握手拒绝休眠，不依赖残留 eventfd。
  const auto next = scheduler.next(true);
  ASSERT_TRUE(next.has_value());
  EXPECT_EQ(*next, queued.handle);
  EXPECT_EQ(wake_.waits.load(), 0u);
  verify_control_return();
}

TEST_F(UringWakeContract, NonblockingConsumeStillRechecksGlobalQueueAfterSleepRegistration) {
  struct actual_waker {
    uring_backend& backend;

    void wake_up() noexcept { backend.wake(); }
  } waker{*backend_};

  faio::runtime::detail::domain_scheduler domain{1};
  faio::runtime::detail::local_scheduler local{
      domain, 0, 1, faio::runtime::detail::worker_waker_ref{waker}};
  auto queued = make_queued_probe();
  ASSERT_FALSE(local.steal_task().has_value());  // 如真实 worker 一样，领取搜索名额后发现空队列。
  domain.enqueue(queued.handle);  // 全局真实队列持有任务；搜索者尚未休眠，暂不另发通知。
  backend_->wake();
  std::array<backend_event, 2> output{};
  ASSERT_EQ(backend_->poll(output, 0), 0);  // 特意先消耗控制 latch，不提前检查全局任务。
  ASSERT_FALSE(control_access::notice(*backend_));
  ASSERT_TRUE(local.prepare_sleep());   // 原状态机登记后，最后搜索者转交通知责任。
  EXPECT_TRUE(local.has_ready_task());  // worker.sleep 每次无限 wait 前必做的真实二次重查。
  EXPECT_TRUE(local.finish_sleep());    // 提前返回也撤销/承接休眠记录，不能再进入 kernel wait。
  const auto next = local.next_task(1);
  ASSERT_TRUE(next.has_value());
  EXPECT_EQ(*next, queued.handle);
  local.before_execute();  // 成对归还搜索名额，保持真实 state_machine 计数完整。
  EXPECT_EQ(wake_.waits.load(), 0u);
  verify_control_return();
}
}  // namespace
#else
TEST(UringControlContract, RequiresNativeLinuxBackend) {
  GTEST_SKIP() << "只在启用 liburing 的 Linux 原生后端验证控制 SQ/CQ";
}
#endif
