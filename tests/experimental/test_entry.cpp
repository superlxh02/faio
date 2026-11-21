/**
 * @file experimental/test_entry.cpp
 * @brief 注解异步入口的任务排空测试。
 */
#include <faio/faio.hpp>
#include <faio/experimental/async_main.h>
#include <atomic>
#include <cstdlib>

namespace {
std::atomic<bool> finished{}; /**< 后台任务完成标记。 */

/** @brief 让出执行权后记录后台任务完成。 */
faio::task<void> background() {
  co_await faio::this_coro::yield();
  finished.store(true);
}

/** @brief 检查进程退出前后台任务已排空。 */
void verify_drain() {
  if (!finished.load())
    std::abort();
}
}  // namespace

/** @brief 注册退出检查并提交后台任务。 */
[[= faio::experimental::main(async_main)]][[= faio::experimental::runtime_options{
    .mode = faio::runtime::mode::multi_thread,
    .workers = 2,
    .exit_policy = faio::experimental::entry_exit_policy::drain}]] faio::task<int>
    async_main(int, char**) {
  if (std::atexit(verify_drain) != 0)
    co_return 1;
  faio::spawn_detached(background());
  co_return 0;
}
