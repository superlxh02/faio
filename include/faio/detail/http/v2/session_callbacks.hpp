#ifndef FAIO_DETAIL_HTTP_SESSION_CALLBACKS_HPP
#define FAIO_DETAIL_HTTP_SESSION_CALLBACKS_HPP

#include <nghttp2/nghttp2.h>
#include <vector>
#include <map>
#include <queue>
#include <charconv>
#include "faio/detail/http/types.hpp"
#include "fastlog/fastlog.hpp"

namespace faio::detail::http {

// =============================================================================
// 客户端会话状态和回调
// =============================================================================
//
// 说明：
// - nghttp2 的回调在协议解析过程中被同步触发。
// - 回调只负责“收集状态”，业务层在会话循环中读取聚合结果。

struct ClientSessionState {
  // 每条 stream 的响应状态
  struct StreamResponse {
    HttpHeaders headers;
    std::vector<uint8_t> body;
    bool headers_complete = false;
    bool body_complete = false;
    int status = 0;
  };

  std::map<int32_t, StreamResponse> responses; // stream_id -> 聚合后的响应
  int32_t current_stream_id = 0;
};

// 客户端：收到 header 就累积到响应里
inline int client_on_header(nghttp2_session* session,
                             const nghttp2_frame* frame,
                             const uint8_t* name, size_t namelen,
                             const uint8_t* value, size_t valuelen,
                             uint8_t flags, void* user_data) {
  // user_data 在建 session 时绑定为 ClientSessionState。
  auto* state = static_cast<ClientSessionState*>(user_data);
  // 头字段属于某一条 stream，必须按 stream 维度聚合。
  int32_t stream_id = frame->hd.stream_id;

  // 首次看到该 stream 时创建聚合槽位。
  if (state->responses.find(stream_id) == state->responses.end()) {
    state->responses[stream_id] = ClientSessionState::StreamResponse{};
  }

  // 回调参数是裸字节指针，按长度构造 string。
  std::string name_str(reinterpret_cast<const char*>(name), namelen);
  std::string value_str(reinterpret_cast<const char*>(value), valuelen);

  // :status 伪头单独处理
  if (name_str == ":status") {
    // :status 必须转成整型状态码供 HttpResponse 构造使用。
    int status = 0;
    auto parse_res = std::from_chars(value_str.data(), value_str.data() + value_str.size(), status);
    state->responses[stream_id].status =
        (parse_res.ec == std::errc()) ? status : 0;
  } else {
    state->responses[stream_id].headers[std::move(name_str)] =
        std::move(value_str);
  }

  return 0;
}

// 客户端：收到数据块就往 body 里拼
inline int client_on_data_chunk_recv(nghttp2_session* session, uint8_t flags,
                                      int32_t stream_id, const uint8_t* data,
                                      size_t len, void* user_data) {
  auto* state = static_cast<ClientSessionState*>(user_data);
  // DATA 可能先于某些头回调到达（异常场景），这里兜底创建状态。
  if (state->responses.find(stream_id) == state->responses.end()) {
    state->responses[stream_id] = ClientSessionState::StreamResponse{};
  }
  // 把 DATA 分片按顺序追加。
  state->responses[stream_id].body.insert(
      state->responses[stream_id].body.end(), data, data + len);
  return 0;
}

// 客户端：stream 关闭时标记这条响应收完了
inline int client_on_stream_close(nghttp2_session* session, int32_t stream_id,
                                   uint32_t error_code, void* user_data) {
  auto* state = static_cast<ClientSessionState*>(user_data);
  // stream 关闭可视作响应接收完成。
  if (state->responses.find(stream_id) != state->responses.end()) {
    state->responses[stream_id].body_complete = true;
  }
  return 0;
}

inline int client_on_frame_send(nghttp2_session* session,
                                const nghttp2_frame* frame,
                                void* user_data) {
  fastlog::console.debug("http client frame send: type={} stream={} flags={}",
                         static_cast<int>(frame->hd.type), frame->hd.stream_id,
                         static_cast<int>(frame->hd.flags));
  return 0;
}

inline int client_on_frame_recv(nghttp2_session* session,
                                const nghttp2_frame* frame,
                                void* user_data) {
  if (frame->hd.type == NGHTTP2_GOAWAY) {
    fastlog::console.error(
        "http client recv GOAWAY: error={} last_stream={} opaque_len={}",
        frame->goaway.error_code, frame->goaway.last_stream_id,
        frame->goaway.opaque_data_len);
  } else if (frame->hd.type == NGHTTP2_RST_STREAM) {
    fastlog::console.error("http client recv RST_STREAM: stream={} error={}",
                           frame->hd.stream_id,
                           frame->rst_stream.error_code);
  } else {
    fastlog::console.debug("http client frame recv: type={} stream={} flags={}",
                           static_cast<int>(frame->hd.type), frame->hd.stream_id,
                           static_cast<int>(frame->hd.flags));
  }
  return 0;
}

inline int client_on_invalid_frame_recv(nghttp2_session* session,
                                        const nghttp2_frame* frame,
                                        int lib_error_code,
                                        void* user_data) {
  fastlog::console.error("http client invalid frame recv: type={} stream={} lib_error={}",
                         static_cast<int>(frame->hd.type), frame->hd.stream_id,
                         lib_error_code);
  return 0;
}

// 组装客户端回调表。
inline nghttp2_session_callbacks* get_client_callbacks() {
  nghttp2_session_callbacks* cbs;
  // 1) 分配回调表对象。
  nghttp2_session_callbacks_new(&cbs);
  // 2) 绑定响应聚合核心回调。
  nghttp2_session_callbacks_set_on_header_callback(cbs,
                                                    client_on_header);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
      cbs, client_on_data_chunk_recv);
  nghttp2_session_callbacks_set_on_stream_close_callback(cbs,
                                                          client_on_stream_close);
  // 3) 绑定诊断与日志回调。
  nghttp2_session_callbacks_set_on_frame_send_callback(cbs,
                                                        client_on_frame_send);
  nghttp2_session_callbacks_set_on_frame_recv_callback(cbs,
                                                        client_on_frame_recv);
  nghttp2_session_callbacks_set_on_invalid_frame_recv_callback(
      cbs, client_on_invalid_frame_recv);
  return cbs;
}

// 释放客户端回调表
inline void free_client_callbacks(nghttp2_session_callbacks* cbs) {
  nghttp2_session_callbacks_del(cbs);
}

// =============================================================================
// 服务端会话状态和回调
// =============================================================================
//
// 说明：
// - 回调侧把 stream 上的 headers/body 聚合为 HttpRequest。
// - 完整请求通过 pending_requests 交给会话主循环消费。

struct ServerSessionState {
  // 每条 stream 的请求状态
  struct StreamRequest {
    HttpMethod method = HttpMethod::GET;
    std::string path;
    HttpHeaders headers;
    std::vector<uint8_t> body;
    bool headers_complete = false;
    bool body_complete = false;
  };

  std::map<int32_t, StreamRequest> requests; // stream_id -> 请求
  std::queue<std::pair<int32_t, HttpRequest>> pending_requests; // (stream_id, 待处理请求)
  int32_t current_stream_id = 0;
};

// 服务端：开始收 headers
inline int server_on_begin_headers(nghttp2_session* session,
                                    const nghttp2_frame* frame,
                                    void* user_data) {
  auto* state = static_cast<ServerSessionState*>(user_data);
  // 新请求开始，记录当前 stream id。
  int32_t stream_id = frame->hd.stream_id;
  state->current_stream_id = stream_id;
  // 首次见到 stream 时分配请求聚合槽位。
  if (state->requests.find(stream_id) == state->requests.end()) {
    state->requests[stream_id] = ServerSessionState::StreamRequest{};
  }
  return 0;
}

// 服务端：每收到一个 header 就塞到请求里
inline int server_on_header(nghttp2_session* session,
                             const nghttp2_frame* frame,
                             const uint8_t* name, size_t namelen,
                             const uint8_t* value, size_t valuelen,
                             uint8_t flags, void* user_data) {
  auto* state = static_cast<ServerSessionState*>(user_data);
  int32_t stream_id = frame->hd.stream_id;

  std::string name_str(reinterpret_cast<const char*>(name), namelen);
  std::string value_str(reinterpret_cast<const char*>(value), valuelen);

  if (state->requests.find(stream_id) == state->requests.end()) {
    state->requests[stream_id] = ServerSessionState::StreamRequest{};
  }

  // 伪头单独处理：method/path 进入结构化字段，其余进 headers。
  if (name_str == ":method") {
    state->requests[stream_id].method = string_to_http_method(value_str);
  } else if (name_str == ":path") {
    state->requests[stream_id].path = std::move(value_str);
  } else {
    state->requests[stream_id].headers[std::move(name_str)] =
        std::move(value_str);
  }

  return 0;
}

// 服务端：完整收到一帧后的回调
inline int server_on_frame_recv(nghttp2_session* session,
                                 const nghttp2_frame* frame, void* user_data) {
  auto* state = static_cast<ServerSessionState*>(user_data);
  fastlog::console.debug("http server frame recv type={} stream={} flags={}",
                         static_cast<int>(frame->hd.type), frame->hd.stream_id,
                         static_cast<int>(frame->hd.flags));

  if (frame->hd.type == NGHTTP2_HEADERS) {
    int32_t stream_id = frame->hd.stream_id;
    if (state->requests.find(stream_id) != state->requests.end()) {
      auto& req = state->requests[stream_id];
      // 头帧到达，标记 headers 已完整可用。
      req.headers_complete = true;
      // 头帧已结束且带 END_STREAM，说明这是无 body 请求。
      if ((frame->hd.flags & NGHTTP2_FLAG_END_HEADERS) &&
          (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        req.body_complete = true;
        HttpRequest http_req(req.method, req.path, req.headers, req.body);
        // 推入待处理队列，由会话主循环串行消费并调用业务 handler。
        state->pending_requests.push({stream_id, http_req});
        fastlog::console.debug("http server queued request stream={} path={}", stream_id,
                               req.path);
      }
    }
  } else if (frame->hd.type == NGHTTP2_DATA) {
    int32_t stream_id = frame->hd.stream_id;
    // DATA 帧在 END_STREAM 时才可判定 body 完整。
    if ((frame->hd.flags & NGHTTP2_FLAG_END_STREAM) &&
        state->requests.find(stream_id) != state->requests.end()) {
      auto& req = state->requests[stream_id];
      req.body_complete = true;
      // 只有 headers 也准备好时才组装 HttpRequest。
      if (req.headers_complete) {
        HttpRequest http_req(req.method, req.path, req.headers, req.body);
        state->pending_requests.push({stream_id, http_req});
        fastlog::console.debug("http server queued data-end request stream={} path={}", stream_id,
                   req.path);
      }
    }
  }
  return 0;
}

// 服务端：把 DATA 帧内容追加到请求 body
inline int server_on_data_chunk_recv(nghttp2_session* session, uint8_t flags,
                                      int32_t stream_id, const uint8_t* data,
                                      size_t len, void* user_data) {
  auto* state = static_cast<ServerSessionState*>(user_data);
  // 防御式创建，保证 insert 一定可执行。
  if (state->requests.find(stream_id) == state->requests.end()) {
    state->requests[stream_id] = ServerSessionState::StreamRequest{};
  }
  // body 分片按到达顺序拼接。
  state->requests[stream_id].body.insert(
      state->requests[stream_id].body.end(), data, data + len);
  return 0;
}

inline int server_on_invalid_frame_recv(nghttp2_session* session,
                                        const nghttp2_frame* frame,
                                        int lib_error_code,
                                        void* user_data) {
  fastlog::console.error("http server invalid frame recv: type={} stream={} lib_error={}",
                         static_cast<int>(frame->hd.type), frame->hd.stream_id,
                         lib_error_code);
  return 0;
}

inline int server_on_frame_send(nghttp2_session* session,
                                const nghttp2_frame* frame,
                                void* user_data) {
  if (frame->hd.type == NGHTTP2_GOAWAY) {
    fastlog::console.error(
        "http server send GOAWAY: error={} last_stream={} opaque_len={}",
        frame->goaway.error_code, frame->goaway.last_stream_id,
        frame->goaway.opaque_data_len);
  } else if (frame->hd.type == NGHTTP2_RST_STREAM) {
    fastlog::console.error("http server send RST_STREAM: stream={} error={}",
                           frame->hd.stream_id,
                           frame->rst_stream.error_code);
  } else {
    fastlog::console.debug("http server frame send: type={} stream={} flags={}",
                           static_cast<int>(frame->hd.type), frame->hd.stream_id,
                           static_cast<int>(frame->hd.flags));
  }
  return 0;
}

// 组装服务端回调表。
inline nghttp2_session_callbacks* get_server_callbacks() {
  nghttp2_session_callbacks* cbs;
  // 1) 分配回调表。
  nghttp2_session_callbacks_new(&cbs);
  // 2) 绑定请求聚合关键回调。
  nghttp2_session_callbacks_set_on_begin_headers_callback(cbs,
                                                          server_on_begin_headers);
  nghttp2_session_callbacks_set_on_header_callback(cbs, server_on_header);
  nghttp2_session_callbacks_set_on_frame_recv_callback(cbs,
                                                        server_on_frame_recv);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
      cbs, server_on_data_chunk_recv);
    // 3) 绑定诊断回调，便于定位协议问题。
  nghttp2_session_callbacks_set_on_invalid_frame_recv_callback(
      cbs, server_on_invalid_frame_recv);
  nghttp2_session_callbacks_set_on_frame_send_callback(cbs,
                                                        server_on_frame_send);
  return cbs;
}

// 释放服务端回调表
inline void free_server_callbacks(nghttp2_session_callbacks* cbs) {
  nghttp2_session_callbacks_del(cbs);
}

} // namespace faio::detail::http

#endif // FAIO_DETAIL_HTTP_SESSION_CALLBACKS_HPP
