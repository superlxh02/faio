#ifndef FAIO_DETAIL_HTTP_V2_SERVER_SESSION_V2_HPP
#define FAIO_DETAIL_HTTP_V2_SERVER_SESSION_V2_HPP

#include <nghttp2/nghttp2.h>
#include <memory>
#include <vector>
#include <functional>
#include <queue>
#include <string>
#include <cstring>
#include "faio/detail/net.hpp"
#include "faio/detail/http/types.hpp"
#include "faio/detail/http/v2/session_callbacks.hpp"
#include "faio/detail/http/v2/nghttp2_util.hpp"
#include "faio/detail/common/error.hpp"
#include "fastlog/fastlog.hpp"

namespace faio::detail::http {

// handler 类型别名
using Handler = std::function<task<HttpResponse>(const HttpRequest&)>;

class Http2ServerSession {
public:
  explicit Http2ServerSession(faio::net::detail::TcpStream stream)
      : _stream(std::move(stream)), _state(std::make_unique<ServerSessionState>()) {
    fastlog::console.debug("http server session created, fd={}", _stream.fd());
  }

  ~Http2ServerSession() {
    if (_session) {
      nghttp2_session_del(_session);
    }
    for (auto* source : _body_sources) {
      delete source;
    }
    _body_sources.clear();
  }

  Http2ServerSession(const Http2ServerSession&) = delete;
  Http2ServerSession& operator=(const Http2ServerSession&) = delete;

  // 禁掉移动，避免会话状态被挪来挪去出问题
  Http2ServerSession(Http2ServerSession&&) = delete;
  Http2ServerSession& operator=(Http2ServerSession&&) = delete;

  // 初始化会话（提交初始 SETTINGS 并刷出控制帧）。
  auto initialize() -> task<expected<void>> {
    // 1) 创建 server session，并绑定回调状态对象。
    nghttp2_session_callbacks* cbs = get_server_callbacks();
    auto res = nghttp2_session_server_new(&_session, cbs, _state.get());
    free_server_callbacks(cbs);

    if (res != 0) {
      co_return std::unexpected(nghttp2_error_to_faio(res));
    }

    // 2) 提交初始 SETTINGS。
    res = nghttp2_submit_settings(_session, NGHTTP2_FLAG_NONE, nullptr, 0);
    if (res != 0) {
      co_return std::unexpected(nghttp2_error_to_faio(res));
    }

    // 3) 把待发送控制帧刷到网络层。
    auto send_res = co_await send_data();
    if (!send_res) {
      co_return std::unexpected(send_res.error());
    }

    fastlog::console.debug("http server session initialized, fd={}", _stream.fd());
    co_return expected<void>();
  }

  // 跑会话主循环：收包 -> 协议解析 -> 派发 handler -> 发送响应。
  auto run(const Handler& handler,
           std::span<const uint8_t> initial_data = {}) -> task<void> {
    fastlog::console.debug("http server session loop start");

    if (!initial_data.empty()) {
      // 连接探测阶段已经读到一段首包，这里先喂给 nghttp2。
      auto init_proc_res = process_received_data(initial_data);
      if (!init_proc_res) {
        fastlog::console.error("http server process initial data error: {}",
                               init_proc_res.error().message());
        co_return;
      }

      // 消费首包阶段已解析出的完整请求。
      while (!_state->pending_requests.empty()) {
        auto [stream_id, req] = _state->pending_requests.front();
        _state->pending_requests.pop();

        // 调业务 handler 得到响应。
        auto response = co_await handler(req);
        auto submit_res = submit_response(stream_id, response);
        if (!submit_res) {
          fastlog::console.error("http server submit response failed: {}",
                                 submit_res.error().message());
          continue;
        }
      }

      // 首包处理后统一 flush 一次。
      auto init_send_res = co_await send_data();
      if (!init_send_res) {
        fastlog::console.error("http server send initial flush error: {}",
                               init_send_res.error().message());
        co_return;
      }
    }

    while (true) {
      // 先从 TCP 读数据
      auto read_res = co_await read_data();
      if (!read_res) {
        // 连接断了或者读出错了
        fastlog::console.debug("http server read finished: {}", read_res.error().message());
        break;
      }

      // 空包视为 EOF
      if (read_res.value().empty()) {
        fastlog::console.debug("http server session eof, fd={}", _stream.fd());
        break;
      }
      fastlog::console.debug("http server recv {} bytes", read_res.value().size());

      // 把收到的数据喂给 nghttp2，回调会持续聚合请求。
      auto proc_res = process_received_data(read_res.value());
      if (!proc_res) {
        // 协议处理出错
        fastlog::console.error("http server process error: {}", proc_res.error().message());
        break;
      }

      // 把当前累计出来的待处理请求全部消费掉。
      while (!_state->pending_requests.empty()) {
        // HTTP/2 是多路复用的，这里每个请求都带自己的 stream_id
        auto [stream_id, req] = _state->pending_requests.front();
        _state->pending_requests.pop();
        fastlog::console.debug("http server handle request stream={} path={}", stream_id,
                   std::string(req.path()));

        // 调业务 handler。
        auto response = co_await handler(req);

        // 响应要按对应 stream_id 发回去，不能串流。
        auto submit_res = submit_response(stream_id, response);
        if (!submit_res) {
          // 这条流提交失败，记日志，继续处理后面的
          fastlog::console.error("http server submit response failed: {}", submit_res.error().message());
          continue;
        }
      }

      // 把待发送的数据刷出去（包含 HEADERS/DATA/控制帧）。
      auto send_res = co_await send_data();
      if (!send_res) {
        fastlog::console.error("http server send error: {}", send_res.error().message());
        break;
      }
    }
    fastlog::console.debug("http server session loop end");
  }

  auto close() -> void {
    _stream.close();
  }

private:
  struct ResponseBodySource {
    std::vector<uint8_t> body;
    size_t offset = 0;
  };

  // 把 HttpResponse 转成 nghttp2 能发的格式。
  // 注意：响应 body 状态由 ResponseBodySource 持有，生命周期需覆盖发送阶段。
  auto submit_response(int32_t stream_id, const HttpResponse& resp)
      -> expected<void> {
    // 先把响应头拼成 nghttp2_nv 数组。
    // 字符串单独存一份，保证 nghttp2 使用期间指针有效。
    std::vector<std::string> string_storage;
    string_storage.reserve(2 + resp.headers().size() * 2);  // :status + headers
    std::vector<nghttp2_nv> nvs;

    // 先塞 :status 伪头（HTTP/2 响应必需）。
    string_storage.emplace_back(":status");
    string_storage.emplace_back(std::to_string(resp.status()));
    nvs.push_back(nghttp2_nv{
      reinterpret_cast<uint8_t*>(string_storage[string_storage.size()-2].data()),
      reinterpret_cast<uint8_t*>(string_storage[string_storage.size()-1].data()),
      string_storage[string_storage.size()-2].size(),
      string_storage[string_storage.size()-1].size(),
      NGHTTP2_NV_FLAG_NONE
    });

    // 再塞普通 headers。
    for (const auto& [name, value] : resp.headers()) {
      string_storage.emplace_back(name);
      string_storage.emplace_back(value);
      nvs.push_back(nghttp2_nv{
        reinterpret_cast<uint8_t*>(string_storage[string_storage.size()-2].data()),
        reinterpret_cast<uint8_t*>(string_storage[string_storage.size()-1].data()),
        string_storage[string_storage.size()-2].size(),
        string_storage[string_storage.size()-1].size(),
        NGHTTP2_NV_FLAG_NONE
      });
    }

    if (!resp.body().empty()) {
      // 有 body 就挂 data provider，nghttp2 会按窗口分块拉数据。
      auto* source_state = new ResponseBodySource{
          std::vector<uint8_t>(resp.body().begin(), resp.body().end()), 0};

      nghttp2_data_provider provider{};
      provider.source.ptr = source_state;
      provider.read_callback = [](nghttp2_session* session, int32_t stream_id,
                                  uint8_t* buf, size_t length,
                                  uint32_t* data_flags, nghttp2_data_source* source,
                                  void* user_data) -> ssize_t {
        auto* state = static_cast<ResponseBodySource*>(source->ptr);
        if (!state) {
          *data_flags |= NGHTTP2_DATA_FLAG_EOF;
          return 0;
        }

        auto remaining = state->body.size() - state->offset;
        if (remaining == 0) {
          *data_flags |= NGHTTP2_DATA_FLAG_EOF;
          return 0;
        }

        // 计算本次可发送字节并拷贝。
        auto to_send = std::min(remaining, length);
        std::memcpy(buf, state->body.data() + state->offset, to_send);
        state->offset += to_send;
        if (state->offset == state->body.size()) {
          // 最后一块发送完成，标记 EOF。
          *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        }
        return static_cast<ssize_t>(to_send);
      };

      auto res = nghttp2_submit_response(_session, stream_id, nvs.data(),
                                         nvs.size(), &provider);
      if (res < 0) {
        delete source_state;
        return std::unexpected(nghttp2_error_to_faio(res));
      }
      _body_sources.push_back(source_state);
    } else {
      // 没 body 就直接发 header-only 响应。
      auto res = nghttp2_submit_response(_session, stream_id, nvs.data(),
                                         nvs.size(), nullptr);
      if (res < 0) {
        return std::unexpected(nghttp2_error_to_faio(res));
      }
    }

    return expected<void>();
  }

  // 从 TCP 流读取一批数据。
  auto read_data() -> task<expected<std::vector<uint8_t>>> {
    std::vector<char> buf(4096);
    // 从 TCP 读取一块原始字节。
    auto res = co_await _stream.read(std::span(buf.data(), buf.size()));
    if (!res) {
      co_return std::unexpected(res.error());
    }
    auto len = res.value();
    // 转成 uint8_t 视图供 nghttp2 接口使用。
    std::vector<uint8_t> result(buf.begin(), buf.begin() + len);
    co_return result;
  }

  // 用 nghttp2 处理收到的数据。
  auto process_received_data(std::span<const uint8_t> data)
      -> expected<void> {
    auto res = nghttp2_session_mem_recv2(_session, data.data(), data.size());
    if (res < 0) {
      return std::unexpected(nghttp2_error_to_faio(res));
    }
    return expected<void>();
  }

  // 把待发送数据写出去。这里先拷贝一份再 co_await，
  // 避免 nghttp2 返回的内部指针跨挂起失效。
  auto send_data() -> task<expected<void>> {
    while (true) {
      const uint8_t* data = nullptr;
      // 让 nghttp2 产出一批待发送字节。
      auto len = nghttp2_session_mem_send2(_session, &data);
      if (len < 0) {
        co_return std::unexpected(nghttp2_error_to_faio(len));
      }
      if (len == 0) {
        // 当前无待发送数据。
        break;
      }

      // 拷贝后再挂起写，防止跨挂起悬空。
      std::vector<uint8_t> copy(data, data + static_cast<size_t>(len));
      auto res = co_await _stream.write_all(
          std::span(reinterpret_cast<const char*>(copy.data()), copy.size()));
      if (!res) {
        co_return std::unexpected(res.error());
      }
    }
    co_return expected<void>();
  }

  faio::net::detail::TcpStream _stream;
  nghttp2_session* _session = nullptr;
  std::unique_ptr<ServerSessionState> _state;
  std::vector<ResponseBodySource*> _body_sources;
};

using ServerSessionAdapter = Http2ServerSession;

} // namespace faio::detail::http

#endif // FAIO_DETAIL_HTTP_V2_SERVER_SESSION_V2_HPP
