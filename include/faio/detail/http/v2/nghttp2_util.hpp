#ifndef FAIO_DETAIL_HTTP_NGHTTP2_UTIL_HPP
#define FAIO_DETAIL_HTTP_NGHTTP2_UTIL_HPP

#include <nghttp2/nghttp2.h>
#include <vector>
#include <string>
#include <cstring>
#include <memory>
#include "faio/detail/http/types.hpp"
#include "faio/detail/common/error.hpp"

namespace faio::detail::http {

// 把 HttpHeaders 转成 nghttp2_nv 数组。
//
// 说明：nghttp2_nv 只存指针，不拥有字符串。
// 本类内部持有字符串副本，保证 nghttp2 使用期间指针有效。
class Headers2Nv {
public:
  explicit Headers2Nv(const HttpHeaders& headers) {
    // 这里只处理普通头，伪头（:method/:path/:status）在调用方单独拼装。
    _nvs.reserve(headers.size());
    for (const auto& [name, value] : headers) {
      // 先构造一个空 nv，后面填充 name/value 指针与长度。
      nghttp2_nv nv{};
      // 复制 name/value 到堆对象，保证 nv 指针在提交期间始终有效。
      auto name_copy = std::make_shared<std::string>(name);
      auto value_copy = std::make_shared<std::string>(value);

      // nghttp2 API 要求 uint8_t*，这里仅做类型适配，不修改字符串内容。
      nv.name = reinterpret_cast<uint8_t*>(name_copy->data());
      nv.namelen = name_copy->size();
      nv.value = reinterpret_cast<uint8_t*>(value_copy->data());
      nv.valuelen = value_copy->size();
      nv.flags = NGHTTP2_NV_FLAG_NONE;

      // 先放入 nv 数组，再保存字符串副本，二者生命周期同属当前对象。
      _nvs.push_back(nv);
      _name_copies.push_back(name_copy);
      _value_copies.push_back(value_copy);
    }
  }

  [[nodiscard]] auto data() noexcept -> nghttp2_nv* {
    return _nvs.empty() ? nullptr : _nvs.data();
  }

  [[nodiscard]] auto size() const noexcept -> size_t {
    return _nvs.size();
  }

private:
  std::vector<nghttp2_nv> _nvs;
  std::vector<std::shared_ptr<std::string>> _name_copies;
  std::vector<std::shared_ptr<std::string>> _value_copies;
};

// HttpMethod 转成 nghttp2 可用字符串。
[[nodiscard]]
inline auto method_to_nghttp2(HttpMethod method) -> std::string {
  return std::string(http_method_to_string(method));
}

// 把 nghttp2 错误码映射成 faio::Error。
[[nodiscard]]
inline auto nghttp2_error_to_faio(int nghttp2_err) -> Error {
  // 非负值表示成功，映射到无错误。
  if (nghttp2_err >= 0) {
    return make_error(0); // 成功
  }

  // 常见 nghttp2 错误码按语义映射到 faio 自定义错误码。
  switch (nghttp2_err) {
  case NGHTTP2_ERR_INVALID_ARGUMENT:
  case NGHTTP2_ERR_BUFFER_ERROR:
  case NGHTTP2_ERR_UNSUPPORTED_VERSION:
    return make_error(static_cast<int>(Error::Http2Protocol));
  case NGHTTP2_ERR_BAD_CLIENT_MAGIC:
    return make_error(static_cast<int>(Error::Http2ExpectedPreface));
  case NGHTTP2_ERR_STREAM_CLOSED:
    return make_error(static_cast<int>(Error::Http2StreamClosed));
  case NGHTTP2_ERR_STREAM_CLOSING:
  case NGHTTP2_ERR_INVALID_STREAM_STATE:
    return make_error(static_cast<int>(Error::Http2StreamReset));
  case NGHTTP2_ERR_REFUSED_STREAM:
    return make_error(static_cast<int>(Error::Http2Refused));
  case NGHTTP2_ERR_INTERNAL:
    return make_error(static_cast<int>(Error::Http2Internal));
  case NGHTTP2_ERR_NOMEM:
    return make_error(ENOMEM);
  default:
    // 未覆盖错误统一收敛到内部错误，避免暴露未定义状态。
    return make_error(static_cast<int>(Error::Http2Internal));
  }
}

// 从 nghttp2_nv 反解出 HttpHeaders。
[[nodiscard]]
inline auto nv_to_headers(const nghttp2_nv* nv, size_t nvlen) -> HttpHeaders {
  HttpHeaders headers;
  for (size_t i = 0; i < nvlen; ++i) {
    // 按 length 构造字符串，避免依赖 '\0' 终止。
    std::string name(reinterpret_cast<const char*>(nv[i].name), nv[i].namelen);
    std::string value(reinterpret_cast<const char*>(nv[i].value), nv[i].valuelen);
    // 后值覆盖前值，符合 map 语义。
    headers[std::move(name)] = std::move(value);
  }
  return headers;
}

} // namespace faio::detail::http

#endif // FAIO_DETAIL_HTTP_NGHTTP2_UTIL_HPP
