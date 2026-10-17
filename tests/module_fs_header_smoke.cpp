#include <faio/fs.hpp>
#include <type_traits>

static_assert(!std::is_copy_constructible_v<faio::fs::File>);
static_assert(std::is_move_constructible_v<faio::fs::File>);

bool module_fs_public_header_smoke() {
  faio::fs::OpenOptions empty;
  auto valid = faio::fs::OpenOptions{}.read().write().flags();
  auto invalid = faio::fs::OpenOptions{}.read().truncate().flags();
  const auto offset = faio::fs::seek_from::current(-7);
  return !empty.flags() && valid.has_value() && !invalid
         && offset.base == faio::fs::seek_from::origin::current && offset.offset == -7;
}
