/**
 * @file custom_awaiter.cpp
 * @brief 将同步完成的操作和外部线程的回调接入 co_await。
 * @details 两个操作都先写入结果，再发出一次完成通知，最后由 on_resume 交付结果。
 */
#include <faio/faio.hpp>

#include <exception>
#include <iostream>
#include <memory>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {
/** @brief 把输入加倍，并在等待恢复后按值交付结果。 */
class doubling_operation : public faio::awaiter_facade<doubling_operation> {
 public:
  /**
   * @brief 保存输入，等待时才执行操作。
   * @param input 一个加倍后仍可用 int 表示的整数。
   */
  explicit doubling_operation(int input) noexcept : input_(input) {}

  /**
   * @brief 登记操作并完成一次同步计算。
   * @tparam Completion faio 提供的完成令牌类型。
   * @param done 保存结果后消耗一次的令牌。发出完成通知后应停止访问操作和借用的数据。
   */
  template <class Completion>
  void on_suspend(Completion done) noexcept {
    result_ = input_ * 2;
    std::move(done).complete();
  }

  /** @return 等待恢复后交付的计算结果。 */
  int on_resume() && noexcept { return result_; }

 private:
  int input_;
  int result_{};
};

/** @brief 直接等待右值操作。完成令牌仅发送通知，结果经 on_resume 返回。 */
faio::task<void> show_custom_awaiter() {
  const int answer = co_await doubling_operation{21};
  if (answer != 42) {
    throw std::logic_error("自定义操作应返回 42");
  }
  std::cout << "同步完成的操作返回 " << answer << '\n';
}

/**
 * @brief 保留可移动的完成令牌，直到外部线程成功接手回调。
 * @tparam Completion faio 向适配操作交付的令牌类型。
 */
template <class Completion>
struct pending_callback {
  explicit pending_callback(Completion&& token) noexcept : done(std::move(token)) {}

  Completion done;                /**< 由外部线程消耗一次；启动失败时由失败处理代码消耗。 */
  std::binary_semaphore armed{0}; /**< 线程被调用方接管后才允许运行回调。 */
};

/** @brief 用线程模拟外部 SDK 的异步回调，借用调用方的输入和线程句柄。 */
class callback_operation {
 public:
  /**
   * @param worker 调用方持有的空线程句柄，在回调结束前保持有效。
   * @param input 回调读取的整数。取回结果后可以释放此输入。
   */
  callback_operation(std::jthread& worker, const int& input) noexcept
      : worker_(worker), input_(input) {}

  /**
   * @brief 登记一次回调，将完成令牌的所有权交给外部线程。
   * @tparam Completion faio 交付的可移动完成令牌。
   * @param done 写好结果后通知等待者的令牌。
   * @details 即使线程启动失败，也先保存异常，再用同一协议完成一次等待。
   */
  template <class Completion>
  void on_suspend(Completion done) noexcept {
    registration_thread_ = std::this_thread::get_id();
    std::shared_ptr<pending_callback<Completion>> pending;
    try {
      pending = std::make_shared<pending_callback<Completion>>(std::move(done));
      worker_ = std::jthread{[this, pending]() noexcept {
        pending->armed.acquire();
        callback_thread_ = std::this_thread::get_id();
        result_ = input_ * 2;
        /** @brief complete 之后等待者可能立即继续，回调此后不再访问操作或输入。 */
        std::move(pending->done).complete();
      }};
    } catch (...) {
      failure_ = std::current_exception();
      if (pending)
        std::move(pending->done).complete();
      else
        std::move(done).complete();
      return;
    }
    pending->armed.release();
  }

  /** @brief 检查完成确实来自外部线程，然后按值交付结果。 */
  int on_resume() && {
    if (failure_)
      std::rethrow_exception(failure_);
    if (registration_thread_ == callback_thread_)
      throw std::logic_error("回调应在登记线程之外执行");
    return result_;
  }

 private:
  std::jthread& worker_;
  const int& input_;
  int result_{};
  std::thread::id registration_thread_;
  std::thread::id callback_thread_;
  std::exception_ptr failure_;
};

/** @brief 用工厂适配普通操作对象，等待外部回调写入结果。 */
faio::task<int> await_callback(std::jthread& worker, const int& input) {
  co_return co_await faio::make_awaiter(callback_operation{worker, input});
}
}  // namespace

/** @brief 运行两种完成方式，由调用线程负责回收外部线程。 */
int main() {
  try {
    faio::block_on(show_custom_awaiter());

    auto input = std::make_unique<const int>(21);
    std::jthread worker;
    const int answer = faio::block_on(await_callback(worker, *input));
    /** @brief 回调在 complete 时结束对输入的借用；即使线程尚未退出，也可以销毁输入。 */
    input.reset();
    if (answer != 42)
      throw std::logic_error("异步回调结果应为 42");
    std::cout << "外部线程完成的操作返回 " << answer << '\n';
  } catch (const std::exception& error) {
    std::cerr << "自定义等待操作失败：" << error.what() << '\n';
    return 1;
  }
}
