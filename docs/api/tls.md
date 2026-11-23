# TLS 接口

配置和握手包含 `<faio/tls/handshake.hpp>`，已建立的流包含 `<faio/tls/stream.hpp>`。接口位于 `faio::tls`。CMake 用 `find_package(faio CONFIG REQUIRED COMPONENTS tls)` 并链接 `faio::tls`，要求 OpenSSL ≥ 3.5.0。`TrustStore::from_openssl_defaults()` 读取 OpenSSL 配置的默认 CA，不读取 macOS Keychain 或 Windows 原生证书存储。

## 接口

| 类别 | 接口 | 行为 |
| --- | --- | --- |
| 配置与身份 | `TrustStore::from_pem` / `from_pem_file` | 解析拥有型非空根证书列表，最多 1 MiB |
| | `TrustStore::from_openssl_defaults()` | 加载 OpenSSL 默认 CA 路径；失败返回错误，不关闭验证 |
| | `Identity::from_pem` / `from_pem_files` | leaf-first 链与匹配私钥；不进行交互式口令输入 |
| | `ServerName::parse` | ASCII DNS 或数字 IPv4/IPv6；DNS 小写、去一个尾点，拒绝端口、zone ID、通配符、Unicode |
| | `ServerName::value()` / `is_ip()` | 规范化名称视图；数字 IP 使用 IP SAN 验证且不发送 DNS SNI |
| | `ClientConfig::builder` / `ServerConfig::builder` | 默认 TLS 1.2-1.3、空 ALPN；用 builder 设置身份、ALPN、版本和客户端认证 |
| | `ClientConfigBuilder::with_identity` | 设置客户端证书链和私钥 |
| | `with_alpn` / `with_versions` | 替换 ALPN 列表；版本只接受 1.2、1.3 且 min ≤ max |
| | `ServerConfigBuilder::with_client_auth` | none/optional/required；optional 允许缺证书，已提供证书仍须有效 |
| | `build()` / `to_builder()` | 验证字段并建立共享 `SSL_CTX` / 从已有配置生成独立草稿 |
| 握手 | `TlsConnector` / `TlsAcceptor` | 只保存共享配置，不进行网络 I/O；`config()` 返回只读副本 |
| | `TlsConnector::with_alpn` | 派生独立 ALPN 列表，复用 `SSL_CTX` 和身份，不修改原 Connector |
| | `connect(ServerName, stream, options)` | 验证链与 DNS/IP 身份、发送必要 SNI、完成握手并 flush 后交付 `TlsStream` |
| | `accept(stream, options)` | 执行服务端握手、mTLS 和 ALPN |
| 加密流 | `read` / `write` | 支持 span 与 `io_buffer`；单次写最多接受 16 KiB 明文，允许短读写 |
| | `flush` / `shutdown` | 排空已接受的密文 / 发送 `close_notify` 并半关闭写方向，读方向仍可继续 |
| | `close` | 默认 5 秒内完成双方通知和底层关闭；有未读应用数据则报 `unread_application_data` |
| | `abort` / `is_terminated` | 非阻塞请求终止 / 查询 Failed、Closed 或已移动状态 |
| | `connection_info()` | 返回 version、cipher、ALPN、verify_result 与可选 peer leaf DER |
| | `into_split()` | 拆成一读一写两个拥有型半边；已有活跃用户操作时抛 `std::logic_error` |
