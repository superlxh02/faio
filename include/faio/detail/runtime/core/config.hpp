#ifndef FAIO_DETAIL_RUNTIME_CONFIG_HPP
#define FAIO_DETAIL_RUNTIME_CONFIG_HPP

#include <format>
#include <thread>

namespace faio::runtime::detail {
static inline constexpr std::size_t MAX_LEVEL{6uz};
static inline constexpr std::size_t SLOT_SIZE{64uz};
static inline constexpr std::size_t SLOT_SHIFT{6uz}; // log2(SLOT_SIZE)
static inline constexpr std::size_t SLOT_MASK{SLOT_SIZE - 1uz}; // SLOT_SIZE - 1
static inline constexpr std::size_t LOCAL_QUEUE_CAPACITY{256uz};

struct Config {
  std::size_t _num_events{1024}; // iouring队列大小
  uint32_t _submit_interval{4};  // 提交间隔
  std::size_t _num_workers{std::thread::hardware_concurrency()}; // 工作线程数量
  uint32_t _io_interval{61};                                     // io间隔
  uint32_t _global_queue_interval{61};                           // 全局队列间隔
};

} // namespace faio::runtime::detail

namespace std {

template <> class formatter<faio::runtime::detail::Config> {
public:
  constexpr auto parse(format_parse_context &context) {
    auto it{context.begin()};
    auto end{context.end()};
    if (it == end || *it == '}') {
      return it;
    }
    ++it;
    if (it != end && *it != '}') {
      throw format_error("Invalid format specifier for Config");
    }
    return it;
  }

  auto format(const faio::runtime::detail::Config &config,
              auto &context) const noexcept {
    return format_to(context.out(),
                     R"(num_events: {},
                         num_workers: {},
                         io_interval: {},
                         global_queue_interval: {},
                         submit_interval: {})",
                     config._num_events, config._num_workers,
                     config._io_interval, config._global_queue_interval,
                     config._submit_interval);
  }
};

} // namespace std

#endif // FAIO_DETAIL_RUNTIME_CONFIG_HPP