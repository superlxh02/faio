#pragma once
#include "faio/detail/io/util/algorithms.hpp"
#include <bit>
#include <cstring>

namespace faio::io {
/** @brief 读取固定宽度整数；字节序转换采用 C++23 std::byteswap。 */
template <std::integral Integer, borrowed_async_reader Reader>
task<expected<Integer>> read_integer(Reader &reader,
                                     std::endian order = std::endian::big) {
  std::array<char, sizeof(Integer)> bytes;
  auto result = co_await read_exact(reader, bytes);
  if (!result)
    co_return std::unexpected{result.error()};
  Integer value;
  std::memcpy(&value, bytes.data(), sizeof(value));
  if (sizeof(Integer) > 1 && order != std::endian::native)
    value = std::byteswap(value);
  co_return value;
}
template <std::integral Integer, borrowed_async_writer Writer>
task<expected<void>> write_integer(Writer &writer, Integer value,
                                   std::endian order = std::endian::big) {
  if (sizeof(Integer) > 1 && order != std::endian::native)
    value = std::byteswap(value);
  std::array<char, sizeof(Integer)> bytes;
  std::memcpy(bytes.data(), &value, sizeof(value));
  co_return co_await write_all(writer, std::span<const char>{bytes});
}
#define FAIO_IO_ENDIAN_INTEGER(Name, Type)                                     \
  template <borrowed_async_reader R> auto read_##Name(R &r) {                  \
    return read_integer<Type>(r);                                              \
  }                                                                            \
  template <borrowed_async_reader R> auto read_##Name##_le(R &r) {             \
    return read_integer<Type>(r, std::endian::little);                         \
  }                                                                            \
  template <borrowed_async_writer W> auto write_##Name(W &w, Type v) {         \
    return write_integer(w, v);                                                \
  }                                                                            \
  template <borrowed_async_writer W> auto write_##Name##_le(W &w, Type v) {    \
    return write_integer(w, v, std::endian::little);                           \
  }
FAIO_IO_ENDIAN_INTEGER(u8, std::uint8_t)
FAIO_IO_ENDIAN_INTEGER(u16, std::uint16_t)
FAIO_IO_ENDIAN_INTEGER(u32, std::uint32_t)
FAIO_IO_ENDIAN_INTEGER(u64, std::uint64_t)
FAIO_IO_ENDIAN_INTEGER(i8, std::int8_t)
FAIO_IO_ENDIAN_INTEGER(i16, std::int16_t)
FAIO_IO_ENDIAN_INTEGER(i32, std::int32_t)
FAIO_IO_ENDIAN_INTEGER(i64, std::int64_t)
#undef FAIO_IO_ENDIAN_INTEGER
} // namespace faio::io
