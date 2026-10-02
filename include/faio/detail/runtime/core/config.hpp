#ifndef FAIO_DETAIL_RUNTIME_CONFIG_HPP
#define FAIO_DETAIL_RUNTIME_CONFIG_HPP

#include <algorithm>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <thread>

namespace faio::runtime::detail {
static inline constexpr std::size_t MAX_LEVEL{6uz};
static inline constexpr std::size_t SLOT_SIZE{64uz};
static inline constexpr std::size_t SLOT_SHIFT{6uz}; // log2(SLOT_SIZE)
static inline constexpr std::size_t SLOT_MASK{SLOT_SIZE - 1uz}; // SLOT_SIZE - 1
static inline constexpr std::size_t LOCAL_QUEUE_CAPACITY{256uz};

// 运行时配置的构建值；runtime_context 在创建线程前验证它。
struct runtime_config {
  std::size_t _num_events{1024}; // iouring队列大小
  uint32_t _submit_interval{4};  // 提交间隔
  std::size_t _num_workers{std::max(1u, std::thread::hardware_concurrency())}; // 工作线程数量
  uint32_t _io_interval{61};                                     // io间隔
  uint32_t _global_queue_interval{61};                           // 全局队列间隔
  uint32_t _idle_spin_count{32}; // 入睡前的有限空闲轮询次数；零表示直接准备休眠。
};

// 验证除数和资源数量；idle_spin_count 为零合法，表示禁用空闲轮询。
inline runtime_config validate_config(runtime_config config) {
  if (config._num_workers == 0 || config._num_events == 0 ||
      config._io_interval == 0 || config._global_queue_interval == 0)
    throw std::invalid_argument("运行时线程数、事件数和调度间隔必须大于零");
  return config;
}

} // namespace faio::runtime::detail

namespace std {

template <> class formatter<faio::runtime::detail::runtime_config> {
public:
  constexpr auto parse(format_parse_context &context) {
    auto it{context.begin()};
    auto end{context.end()};
    if (it == end || *it == '}') {
      return it;
    }
    ++it;
    if (it != end && *it != '}') {
      throw format_error("Invalid format specifier for runtime_config");
    }
    return it;
  }

  auto format(const faio::runtime::detail::runtime_config &config,
              auto &context) const noexcept {
    return format_to(context.out(),
                     R"(num_events: {},
                         num_workers: {},
                         io_interval: {},
                         global_queue_interval: {},
                         submit_interval: {},
                         idle_spin_count: {})",
                     config._num_events, config._num_workers,
                     config._io_interval, config._global_queue_interval,
                     config._submit_interval, config._idle_spin_count);
  }
};

} // namespace std

#endif // FAIO_DETAIL_RUNTIME_CONFIG_HPP
