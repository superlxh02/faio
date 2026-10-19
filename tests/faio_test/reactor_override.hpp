#pragma once

// 只供 FAIO_TEST_BACKEND_INJECTION 的单 TU 合同目标使用。
namespace faio::io::detail {
class test_reactor_interface {
 public:
  virtual ~test_reactor_interface() = default;
  virtual int register_resource(int fd, std::uint64_t key) noexcept = 0;
  virtual void unregister_resource(int fd) noexcept = 0;
  virtual int poll(std::span<readiness_event> out, std::optional<int> timeout) noexcept = 0;
  virtual void wake() const noexcept = 0;
  virtual const char* name() const noexcept = 0;
};

template <reactor_implementation R>
class test_reactor_model final : public test_reactor_interface {
 public:
  explicit test_reactor_model(std::unique_ptr<R> object) : object_(std::move(object)) {}

  int register_resource(int fd, std::uint64_t key) noexcept override {
    return object_.register_resource(fd, key);
  }

  void unregister_resource(int fd) noexcept override { return object_.unregister_resource(fd); }

  int poll(std::span<readiness_event> out, std::optional<int> timeout) noexcept override {
    return object_.poll(out, timeout);
  }

  void wake() const noexcept override { return object_.wake(); }

  const char* name() const noexcept override { return object_.name(); }

 private:
  basic_reactor_box<R> object_;
};

class reactor_box {
 public:
  template <reactor_implementation R>
  explicit reactor_box(std::unique_ptr<R> object)
      : object_(std::make_unique<test_reactor_model<R>>(std::move(object))) {}

  reactor_box(reactor_box&&) noexcept = default;
  reactor_box& operator=(reactor_box&&) = delete;
  reactor_box(const reactor_box&) = delete;
  reactor_box& operator=(const reactor_box&) = delete;
  ~reactor_box() = default;

  int register_resource(int fd, std::uint64_t key) noexcept {
    return object_->register_resource(fd, key);
  }

  void unregister_resource(int fd) noexcept { return object_->unregister_resource(fd); }

  int poll(std::span<readiness_event> out, std::optional<int> timeout) noexcept {
    return object_->poll(out, timeout);
  }

  void wake() const noexcept { return object_->wake(); }

  const char* name() const noexcept { return object_->name(); }

 private:
  std::unique_ptr<test_reactor_interface> object_;
};
}  // namespace faio::io::detail
