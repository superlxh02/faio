#include <faio/io.hpp>
#include <faio/detail/io/engine.hpp>
#include <faio/detail/io/traits.hpp>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

static_assert(faio::io::async_reader<faio::io::MemoryStream>);
static_assert(faio::io::async_writer<faio::io::MemoryStream>);
static_assert(faio::io::borrowed_async_reader<faio::io::MemoryStream>);
static_assert(faio::io::borrowed_async_writer<faio::io::MemoryStream>);
static_assert(!std::is_copy_constructible_v<faio::io::io_engine>);
static_assert(sizeof(faio::io::io_engine) == sizeof(std::shared_ptr<faio::io::detail::io_domain>));

bool module_io_public_header_smoke() {
  constexpr std::string_view payload = "public io entry";
  auto original = faio::io::io_buffer::copy(payload);
  const auto* storage = original.data();
  auto moved = std::move(original);
  return original.empty() && original.capacity() == 0 && moved.data() == storage
         && std::string_view{moved.data(), moved.size()} == payload;
}
