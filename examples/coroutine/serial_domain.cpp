/**
 * @file serial_domain.cpp
 * @brief 在整项更新操作期间保持独占，包括协程挂起时。
 * @details 两项更新并发提交到同一串行域。关闭时等待已接纳的更新结束，关闭后的提交被拒绝。
 */
#include <faio/faio.hpp>

#include <atomic>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
/**
 * @brief 先暂停第一笔更新，再从域外发起第二笔更新。
 * @param domain 两笔更新共用的串行域。
 * @param balance 两个任务共享的余额，使用原子读写检测丢失更新。
 * @details 第一笔更新在读取余额后、写回之前挂起。第二笔进入域时应能看到第一笔已写回的余额。
 */
faio::task<void> update_across_suspension(faio::serial_domain<>& domain,
                                          std::atomic<int>& balance) {
  faio::sync::latch first_entered{1};
  faio::sync::latch begin_second{1};
  auto first = domain.run([&]() -> faio::task<void> {
    const int before = balance.load();
    first_entered.count_down();
    co_await faio::this_coro::yield();
    co_await begin_second.wait();
    balance.store(before + 20);
  });
  const auto submit_second = [&]() -> faio::task<void> {
    co_await first_entered.wait();
    auto second = domain.run([&]() -> faio::task<void> {
      const int before = balance.load();
      if (before != 20)
        throw std::logic_error("第二笔更新进入时第一笔尚未写回");
      balance.store(before + 22);
      co_return;
    });
    /** @brief 从域外允许第一笔继续。第二笔获得独占之后才会执行任务体。 */
    begin_second.count_down();
    co_await std::move(second);
  };
  co_await faio::join(std::move(first), submit_second());
}

/**
 * @brief 确认一项更新已进入串行域，再演示从域外关闭。
 * @param domain 与前两次并发更新相同的串行域。
 * @param balance 在最后一项更新完成后供关闭方检查。
 */
faio::task<void> close_during_update(faio::serial_domain<>& domain, std::atomic<int>& balance) {
  faio::sync::latch entered{1};
  faio::sync::latch finish_update{1};
  faio::sync::latch closing_requested{1};
  co_await faio::scope([&](faio::scope_context& tasks) -> faio::task<void> {
    tasks.spawn(domain.run([&]() -> faio::task<void> {
      entered.count_down();
      co_await finish_update.wait();
      ++balance;
    }));
    co_await entered.wait();

    const auto close = [&]() -> faio::task<void> {
      closing_requested.count_down();
      co_await domain.close();
      /** @brief close 返回后，这项已接纳的更新必须完成。 */
      if (balance != 43)
        throw std::logic_error("关闭早于已接纳的更新退出");
    };
    const auto finish = [&]() -> faio::task<void> {
      co_await closing_requested.wait();
      /** @brief 域外任务允许当前更新结束。域内任务不能等待关闭流程，因为关闭需要先释放域的独占权。 */
      finish_update.count_down();
    };
    co_await faio::join(close(), finish());
  });
}

/** @brief 验证余额不会因更新交错而丢失，并检查关闭后的提交。 */
faio::task<void> show_serial_domain() {
  faio::serial_domain domain;
  std::atomic<int> balance{0};
  co_await update_across_suspension(domain, balance);
  if (balance != 42)
    throw std::logic_error("并发更新发生覆盖");
  std::cout << "两项跨挂起的更新均已提交，余额为 " << balance.load() << '\n';

  co_await close_during_update(domain, balance);
  try {
    co_await domain.run([]() -> faio::task<void> { co_return; });
  } catch (const std::logic_error&) {
    std::cout << "关闭已排空更新，余额为 " << balance.load() << "，后续提交已被拒绝\n";
    co_return;
  }
  throw std::logic_error("关闭后的串行域仍接纳了新工作");
}
}  // namespace

/** @brief 等待串行域示例结束，失败时打印诊断。 */
int main() {
  try {
    faio::block_on(show_serial_domain());
  } catch (const std::exception& error) {
    std::cerr << "串行域示例失败：" << error.what() << '\n';
    return 1;
  }
}
