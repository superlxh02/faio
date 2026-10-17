#pragma once

// 只供 FAIO_TEST_BACKEND_INJECTION 的单 TU 合同目标使用。
namespace faio::io::detail {
class test_backend_interface {
 public:
  virtual ~test_backend_interface() = default;
  virtual int attach(native_registration h, std::uint64_t key) noexcept = 0;
  virtual void detach(native_registration h) noexcept = 0;
  virtual backend_submit_result try_submit(backend_operation op) noexcept = 0;
  virtual backend_flush_result flush() noexcept = 0;
  virtual int poll(std::span<backend_event> out, std::optional<int> timeout) noexcept = 0;
  virtual void request_cancel(backend_operation op) noexcept = 0;
  virtual void wake() const noexcept = 0;
  virtual void begin_shutdown() noexcept = 0;
  virtual bool begin_failure(int error) noexcept = 0;
  virtual bool quiescent() const noexcept = 0;
  virtual bool native() const noexcept = 0;
  virtual bool poll_flushes_submissions() const noexcept = 0;
  virtual bool supports(std::uint32_t kind) const noexcept = 0;
  virtual const char* name() const noexcept = 0;
  virtual backend_statistics statistics() const noexcept = 0;
};

template <backend_implementation B>
class test_backend_model final : public test_backend_interface {
 public:
  explicit test_backend_model(basic_backend_box<B> object) : object_(std::move(object)) {}

  int attach(native_registration h, std::uint64_t key) noexcept override {
    return object_.attach(h, key);
  }

  void detach(native_registration h) noexcept override { return object_.detach(h); }

  backend_submit_result try_submit(backend_operation op) noexcept override {
    return object_.try_submit(op);
  }

  backend_flush_result flush() noexcept override { return object_.flush(); }

  int poll(std::span<backend_event> out, std::optional<int> timeout) noexcept override {
    return object_.poll(out, timeout);
  }

  void request_cancel(backend_operation op) noexcept override { return object_.request_cancel(op); }

  void wake() const noexcept override { return object_.wake(); }

  void begin_shutdown() noexcept override { return object_.begin_shutdown(); }

  bool begin_failure(int error) noexcept override { return object_.begin_failure(error); }

  bool quiescent() const noexcept override { return object_.quiescent(); }

  bool native() const noexcept override { return object_.native(); }

  bool poll_flushes_submissions() const noexcept override {
    return object_.poll_flushes_submissions();
  }

  bool supports(std::uint32_t kind) const noexcept override { return object_.supports(kind); }

  const char* name() const noexcept override { return object_.name(); }

  backend_statistics statistics() const noexcept override { return object_.statistics(); }

 private:
  basic_backend_box<B> object_;
};

class backend_box {
 public:
  template <backend_implementation B>
  explicit backend_box(std::unique_ptr<B> object)
      : backend_box(basic_backend_box<B>{std::move(object)}) {}

  template <backend_implementation B>
  explicit backend_box(basic_backend_box<B> object)
      : object_(std::make_unique<test_backend_model<B>>(std::move(object))) {}

  backend_box(backend_box&&) noexcept = default;
  backend_box& operator=(backend_box&&) = delete;
  backend_box(const backend_box&) = delete;
  backend_box& operator=(const backend_box&) = delete;
  ~backend_box() = default;

  int attach(native_registration h, std::uint64_t key) noexcept { return object_->attach(h, key); }

  void detach(native_registration h) noexcept { return object_->detach(h); }

  backend_submit_result try_submit(backend_operation op) noexcept {
    return object_->try_submit(op);
  }

  backend_flush_result flush() noexcept { return object_->flush(); }

  int poll(std::span<backend_event> out, std::optional<int> timeout) noexcept {
    return object_->poll(out, timeout);
  }

  void request_cancel(backend_operation op) noexcept { return object_->request_cancel(op); }

  void wake() const noexcept { return object_->wake(); }

  void begin_shutdown() noexcept { return object_->begin_shutdown(); }

  bool begin_failure(int error) noexcept { return object_->begin_failure(error); }

  bool quiescent() const noexcept { return object_->quiescent(); }

  bool native() const noexcept { return object_->native(); }

  bool poll_flushes_submissions() const noexcept { return object_->poll_flushes_submissions(); }

  bool supports(std::uint32_t kind) const noexcept { return object_->supports(kind); }

  const char* name() const noexcept { return object_->name(); }

  backend_statistics statistics() const noexcept { return object_->statistics(); }

  int attach(int fd, std::uint64_t key) noexcept {
    return attach({static_cast<std::uintptr_t>(fd), native_handle_kind::posix_descriptor}, key);
  }

  void detach(int fd) noexcept {
    detach({static_cast<std::uintptr_t>(fd), native_handle_kind::posix_descriptor});
  }

 private:
  std::unique_ptr<test_backend_interface> object_;
};
}  // namespace faio::io::detail
