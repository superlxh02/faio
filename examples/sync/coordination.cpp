/**
 * @file examples/sync/coordination.cpp
 * @brief 同步下篇：等待初始化、协调分轮工作，通过有界通道汇集结果。
 */
#include <faio/faio.hpp>
#include <array>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using result_channel = faio::sync::mpsc<int>;

/**
 * @brief 写入两个分片的输入值，再开放一次性的启动信号。
 * @param inputs 每个分片使用各自的输入值，初始化后只读。
 * @param ready 初始计数为 1，归零后允许两个分片继续。
 */
faio::task<void> prepare(std::array<int, 2>& inputs, faio::sync::latch& ready) {
  inputs = {10, 11};
  ready.count_down();
  co_return;
}

/**
 * @brief 等待输入，然后按两轮发送各自的结果。
 * @param ready 一次性的输入就绪信号。
 * @param phase 每一轮需要两个分片都到达才能继续。
 * @param input 当前分片使用的输入值，只有 ready 归零后才读取。
 * @param output 当前分片独立持有的发送端。
 */
faio::task<void> produce(faio::sync::latch& ready,
                         faio::sync::barrier& phase,
                         const int& input,
                         result_channel::sender output) {
  co_await ready.wait();
  for (const int factor : {1, 2}) {
    const int result = input * factor;
    /** @brief 通道容量为一。通道满时，发送协程挂起，由正在运行的消费者释放空间。 */
    const auto sent = co_await output.send(result);
    if (!sent)
      throw std::runtime_error("发送结果失败：" + std::string{sent.error().message()});
    co_await phase.arrive_and_wait();
  }
  output.close(); /**< 最后一个发送端关闭后，接收端排空队列并观察到关闭。 */
}

/**
 * @brief 独占接收端，消费两轮结果直到通道关闭。
 * @param input 移入任务的唯一接收端。
 * @return 收到的四个结果之和；通道提前关闭或收到额外结果时报告错误。
 */
faio::task<int> consume(result_channel::receiver input) {
  int count = 0;
  int total = 0;
  for (;;) {
    const auto result = co_await input.recv();
    if (!result) {
      if (result.error().value() != faio::error::CLOSED_CHANNEL)
        throw std::runtime_error("接收结果失败：" + std::string{result.error().message()});
      break;
    }
    ++count;
    total += *result;
  }
  if (count != 4)
    throw std::runtime_error("收到的结果数量错误");
  co_return total;
}

/** @brief 保持同步对象和输入存活，等待初始化、两个分片与消费者全部退出。 */
faio::task<void> demonstrate() {
  std::array<int, 2> inputs{};
  faio::sync::latch ready{1};
  faio::sync::barrier phase{2};
  auto [sender, receiver] = result_channel::make(1);
  auto second_sender = sender;

  /** @brief 任一任务失败，任务组请求同组的等待者停止，并在返回前等它们退出。 */
  auto result = co_await faio::task_group([&](faio::task_group_context& group) -> faio::task<int> {
    (void)group.spawn_observed(prepare(inputs, ready));
    /** @brief 移走局部发送端，让消费者能在两个生产者关闭后观察到通道关闭。 */
    (void)group.spawn_observed(produce(ready, phase, inputs[0], std::move(sender)));
    (void)group.spawn_observed(produce(ready, phase, inputs[1], std::move(second_sender)));
    auto collector = group.spawn_observed(consume(std::move(receiver)));
    co_return (co_await collector).value_or_throw();
  });
  const int total = std::move(result.body).value_or_throw();
  if (total != 63)
    throw std::runtime_error("两个分片的汇总结果错误");
  faio::log::logger()->info("全部分片处理完毕，结果之和为 {}", total);
}

}  // namespace

/** @brief 等待分轮处理和结果汇总，失败时返回非零状态。 */
int main() {
  try {
    faio::block_on(demonstrate());
    return 0;
  } catch (const std::exception& error) {
    faio::log::logger()->error("任务协调示例失败：{}", error.what());
    return 1;
  }
}
