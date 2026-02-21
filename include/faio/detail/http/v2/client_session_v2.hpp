#ifndef FAIO_DETAIL_HTTP_V2_CLIENT_SESSION_V2_HPP
#define FAIO_DETAIL_HTTP_V2_CLIENT_SESSION_V2_HPP

#include "faio/detail/common/error.hpp"
#include "faio/detail/http/v2/nghttp2_util.hpp"
#include "faio/detail/http/v2/session_callbacks.hpp"
#include "faio/detail/http/types.hpp"
#include "faio/detail/net.hpp"
#include "fastlog/fastlog.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <nghttp2/nghttp2.h>
#include <span>
#include <string>
#include <vector>

namespace faio::detail::http {

class Http2ClientSession {
public:
  explicit Http2ClientSession(faio::net::detail::TcpStream stream)
      : _stream(std::move(stream)),
        _state(std::make_unique<ClientSessionState>()) {}

  ~Http2ClientSession() {
    if (_session) {
      nghttp2_session_del(_session);
    }
  }

  Http2ClientSession(const Http2ClientSession &) = delete;
  Http2ClientSession &operator=(const Http2ClientSession &) = delete;

  // 禁掉移动，避免会话状态被意外挪走
  Http2ClientSession(Http2ClientSession &&) = delete;
  Http2ClientSession &operator=(Http2ClientSession &&) = delete;

  // 初始化会话（提交初始 SETTINGS 并刷出待发送控制帧）。
  auto initialize() -> task<expected<void>> {
    // 1) 构造回调表，并创建客户端 session。
    auto cbs = get_client_callbacks();
    auto res = nghttp2_session_client_new(&_session, cbs, _state.get());
    free_client_callbacks(cbs);

    if (res != 0) {
      co_return std::unexpected(nghttp2_error_to_faio(res));
    }

    // 2) 提交初始 SETTINGS（HTTP/2 握手阶段必需控制帧）。
    res = nghttp2_submit_settings(_session, NGHTTP2_FLAG_NONE, nullptr, 0);
    if (res != 0) {
      co_return std::unexpected(nghttp2_error_to_faio(res));
    }

    // 3) 把待发送控制帧刷到网络层。
    auto send_res = co_await send_data();
    if (!send_res) {
      co_return std::unexpected(send_res.error());
    }

    co_return expected<void>();
  }

  // 发请求并等待完整响应。
  // 响应聚合由 session_callbacks 中的 ClientSessionState 负责。
  auto request(const HttpRequest &req) -> task<expected<HttpResponse>> {
    fastlog::console.debug("http client submit request: method={} path={}",
                           std::string(http_method_to_string(req.method())),
                           std::string(req.path()));
    // 把字符串单独存起来，保证 nghttp2 持有的指针始终有效。
    // 提前 reserve，尽量避免扩容导致内部地址变化。
    std::vector<std::string> string_storage;
    string_storage.reserve(8 +
                           req.headers().size() *
                               2); // 4 个伪头 * 2 + 自定义头 * 2

    // 组装 nghttp2_nv 数组。
    std::vector<nghttp2_nv> nvs;

    // HTTP/2 必需伪头。
    // :method
    string_storage.emplace_back(":method");
    string_storage.emplace_back(http_method_to_string(req.method()));
    nvs.push_back(
        nghttp2_nv{reinterpret_cast<uint8_t *>(
                       string_storage[string_storage.size() - 2].data()),
                   reinterpret_cast<uint8_t *>(
                       string_storage[string_storage.size() - 1].data()),
                   string_storage[string_storage.size() - 2].size(),
                   string_storage[string_storage.size() - 1].size(),
                   NGHTTP2_NV_FLAG_NONE});

    // :path
    string_storage.emplace_back(":path");
    string_storage.emplace_back(req.path());
    nvs.push_back(
        nghttp2_nv{reinterpret_cast<uint8_t *>(
                       string_storage[string_storage.size() - 2].data()),
                   reinterpret_cast<uint8_t *>(
                       string_storage[string_storage.size() - 1].data()),
                   string_storage[string_storage.size() - 2].size(),
                   string_storage[string_storage.size() - 1].size(),
                   NGHTTP2_NV_FLAG_NONE});

    // :scheme
    string_storage.emplace_back(":scheme");
    string_storage.emplace_back("http");
    nvs.push_back(
        nghttp2_nv{reinterpret_cast<uint8_t *>(
                       string_storage[string_storage.size() - 2].data()),
                   reinterpret_cast<uint8_t *>(
                       string_storage[string_storage.size() - 1].data()),
                   string_storage[string_storage.size() - 2].size(),
                   string_storage[string_storage.size() - 1].size(),
                   NGHTTP2_NV_FLAG_NONE});

    // :authority
    string_storage.emplace_back(":authority");
    string_storage.emplace_back("localhost");
    nvs.push_back(
        nghttp2_nv{reinterpret_cast<uint8_t *>(
                       string_storage[string_storage.size() - 2].data()),
                   reinterpret_cast<uint8_t *>(
                       string_storage[string_storage.size() - 1].data()),
                   string_storage[string_storage.size() - 2].size(),
                   string_storage[string_storage.size() - 1].size(),
                   NGHTTP2_NV_FLAG_NONE});

    // 再追加普通请求头。
    for (const auto &[name, value] : req.headers()) {
      string_storage.emplace_back(name);
      string_storage.emplace_back(value);
      nvs.push_back(
          nghttp2_nv{reinterpret_cast<uint8_t *>(
                         string_storage[string_storage.size() - 2].data()),
                     reinterpret_cast<uint8_t *>(
                         string_storage[string_storage.size() - 1].data()),
                     string_storage[string_storage.size() - 2].size(),
                     string_storage[string_storage.size() - 1].size(),
                     NGHTTP2_NV_FLAG_NONE});
    }

    struct RequestBodySource {
      std::span<const uint8_t> data;
      size_t offset = 0;
    };

    RequestBodySource body_source{req.body(), 0};
    nghttp2_data_provider data_provider{};

    if (!body_source.data.empty()) {
      // 非空 body 时挂 data provider，按需分块供 nghttp2 拉取。
      data_provider.source.ptr = &body_source;
      data_provider.read_callback =
          [](nghttp2_session *, int32_t, uint8_t *buf, size_t length,
             uint32_t *data_flags, nghttp2_data_source *source,
             void *) -> ssize_t {
        auto *body = static_cast<RequestBodySource *>(source->ptr);
        // 计算剩余可发送字节。
        auto remaining = body->data.size() - body->offset;
        auto to_copy = std::min(length, remaining);
        if (to_copy > 0) {
          // 把当前分片拷贝到 nghttp2 提供的发送缓冲。
          std::memcpy(buf, body->data.data() + body->offset, to_copy);
          body->offset += to_copy;
        }
        if (body->offset >= body->data.size()) {
          // 已发送完全部 body，标记 EOF。
          *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        }
        return static_cast<ssize_t>(to_copy);
      };
    }

    // 提交请求，拿到 stream_id（后续按 stream 维度等待响应）。
    auto stream_id = nghttp2_submit_request(
        _session, nullptr, nvs.data(), nvs.size(),
        body_source.data.empty() ? nullptr : &data_provider, nullptr);
    if (stream_id < 0) {
      co_return std::unexpected(nghttp2_error_to_faio(stream_id));
    }
    fastlog::console.debug("http client stream id={}", stream_id);

    // 先把本次请求相关帧刷出去（HEADERS / 可选 DATA）。
    auto send_res = co_await send_data();
    if (!send_res) {
      co_return std::unexpected(send_res.error());
    }

    // 循环读取响应数据并喂给 nghttp2，直到该 stream 响应完整。
    std::vector<uint8_t> buf(8192);
    for (int i = 0; i < 2048; i++) {
      auto read_res = co_await _stream.read(
          std::span(reinterpret_cast<char *>(buf.data()), buf.size()));
      if (!read_res) {
        co_return std::unexpected(read_res.error());
      }

      size_t len = read_res.value();
      if (len == 0) {
        fastlog::console.warn("http client got eof before response complete");
        break;
      }
      fastlog::console.debug("http client recv {} bytes", len);

      // 把收到的数据交给 nghttp2 解析，触发回调聚合响应。
      auto proc_res = nghttp2_session_mem_recv2(_session, buf.data(), len);
      if (proc_res < 0) {
        co_return std::unexpected(nghttp2_error_to_faio(proc_res));
      }

      // 顺便把待发送控制帧（如 WINDOW_UPDATE）刷掉。
      auto s = co_await send_data();
      if (!s) {
        co_return std::unexpected(s.error());
      }

      // 检查这条 stream 的响应是否已在回调侧聚合完成。
      auto it = _state->responses.find(stream_id);
      if (it != _state->responses.end() && it->second.body_complete) {
        // 回调侧已聚合 status/headers/body，这里直接构造业务响应。
        HttpResponse http_resp(it->second.status, it->second.headers,
                               it->second.body);
        fastlog::console.info(
            "http client response complete: status={} body={} bytes",
            http_resp.status(), http_resp.body().size());
        co_return http_resp;
      }

      if (!nghttp2_session_want_read(_session) &&
          !nghttp2_session_want_write(_session)) {
        // 协议层也不想再读写了，提前结束循环
        break;
      }
    }

    // 循环退出仍未完整，按超时返回。
    co_return std::unexpected(make_error(ETIMEDOUT));
  }

  // 返回 close 的 awaiter；外层一定要 co_await，
  // 不然临时对象提前析构可能触发悬空问题。
  auto close() noexcept { return _stream.close(); }

private:
  // 刷待发送数据。先把 nghttp2 的输出拷贝出来再 co_await：
  // nghttp2 给的指针只在下一次 nghttp2 调用前有效，
  // 挂起期间如果又触发 recv，那个指针就可能失效。
  auto send_data() -> task<expected<void>> {
    while (true) {
      const uint8_t *data = nullptr;
      // 让 nghttp2 产出一批待发送字节。
      auto len = nghttp2_session_mem_send2(_session, &data);
      if (len < 0) {
        co_return std::unexpected(nghttp2_error_to_faio(len));
      }
      if (len == 0) {
        // 没有待发送数据，发送阶段结束。
        break;
      }

      // 复制到独立 buffer 后再 co_await，避免悬空指针。
      std::vector<uint8_t> copy(data, data + static_cast<size_t>(len));
      auto res = co_await _stream.write_all(
          std::span(reinterpret_cast<const char *>(copy.data()), copy.size()));
      if (!res) {
        co_return std::unexpected(res.error());
      }
    }
    co_return expected<void>();
  }

  faio::net::detail::TcpStream _stream;
  nghttp2_session *_session = nullptr;
  std::unique_ptr<ClientSessionState> _state;
};

using ClientSessionAdapter = Http2ClientSession;

} // namespace faio::detail::http

#endif // FAIO_DETAIL_HTTP_V2_CLIENT_SESSION_V2_HPP
