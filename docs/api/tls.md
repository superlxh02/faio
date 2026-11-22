# TLS 接口

[查询 TLS 类型与方法 →](../../api/group__tls.html)

配置并启动握手时包含 `<faio/tls/handshake.hpp>`，使用已建立的 TLS 流时包含 `<faio/tls/stream.hpp>`。接口位于 `faio::tls`，消费目标链接 `faio::tls`。加密使用 OpenSSL 的内存 BIO，底层读写交给 faio 传输。基础 `faio::faio` 与 `<faio/net/tcp.hpp>` 的依赖保持原有范围。

```cmake
find_package(faio CONFIG REQUIRED COMPONENTS tls)
target_link_libraries(app PRIVATE faio::tls)
```

配置要求 OpenSSL ≥ 3.5.0。`TrustStore::from_openssl_defaults()` 表示 OpenSSL 配置的默认 CA 文件/目录，不表示 macOS Keychain 或 Windows 原生证书存储。跨平台部署可显式提供 PEM 根库。

## 配置、身份和名称

以下工厂均同步执行，返回 `faio::expected<T>`；应在启动配置阶段调用。文件读取错误保留 POSIX/Win32 来源，PEM、证书和 TLS 配置错误使用 `error_domain::tls`。

| 类型/方法 | 合同 |
| --- | --- |
| `TrustStore::from_pem(std::string)` | 解析拥有型、非空根证书列表，最多 1 MiB |
| `TrustStore::from_pem_file(path)` | 同步读取文件，再执行相同校验 |
| `TrustStore::from_openssl_defaults()` | 加载 OpenSSL 默认 CA 路径；失败返回错误，不关闭验证 |
| `Identity::from_pem(chain, key)` | leaf-first 链和匹配私钥；验证每个相邻 issuer/签名；不进行交互式口令输入 |
| `Identity::from_pem_files(chain_path, key_path)` | 同步读取证书链和私钥文件 |
| `ServerName::parse(std::string)` | ASCII DNS 或不带方括号的数字 IPv4/IPv6；DNS 小写、去一个尾点；拒绝端口、zone ID、通配符、空 label 和 Unicode |
| `ServerName::value()` | 返回规范化名称的借用视图 |
| `ServerName::is_ip()` | 数字 IP 使用 IP SAN 验证且不发送 DNS SNI |

TrustStore、Identity、ClientConfig、ServerConfig、TlsConnector 和 TlsAcceptor 可复制，共享不可变配置；构造有效配置必须经过工厂，没有默认构造。ServerName 是拥有型可复制值。getter 的视图随所属对象销毁、移动或赋值失效。移动后的配置/名称仅保证析构、赋值和复制空状态安全；异步入口遇到空配置返回 `EBADF`。

```cpp
auto roots = faio::tls::TrustStore::from_pem_file("ca.pem");
if (!roots) { /* 处理 roots.error() */ }
auto draft = faio::tls::ClientConfig::builder(*roots);
draft.with_alpn({"http/1.1"});
auto config = draft.build();
if (!config) { /* 处理 config.error() */ }
faio::tls::TlsConnector connector{std::move(*config)};
```

| Builder/Config API | 说明 |
| --- | --- |
| `ClientConfig::builder(TrustStore)` | 初始无客户端身份、空 ALPN、TLS 1.2–1.3 |
| `ServerConfig::builder(Identity)` | 初始不要求客户端证书、空 ALPN、TLS 1.2–1.3 |
| `to_builder()` | 独立草稿；build 新配置不修改已有配置 |
| `ClientConfigBuilder::with_identity(Identity)` | 设置客户端证书链和私钥 |
| 两个 builder 的 `with_alpn(vector<string>)` | 替换列表；每项 1–255 字节、无重复、总 wire 长度 ≤65535；名称允许 NUL，按字节精确匹配 |
| 两个 builder 的 `with_versions(min, max)` | 仅接受 `TlsVersion::v1_2/v1_3` 且 min≤max |
| `ServerConfigBuilder::with_client_auth(mode, TrustStore)` | none/optional/required；optional 允许缺证书，已提供证书仍必须有效 |
| `ServerConfigBuilder::without_client_auth()` | 重置为 none 并清除客户端根库 |
| 两个 builder 的 `build() const` | 验证字段，建立共享 SSL_CTX；失败不返回半配置对象 |
| Config 的 `min_version/max_version/alpn_protocols` | 只读查询；ALPN 返回 `span<const string>` |
| `ServerConfig::client_auth()` | 返回客户端认证模式 |

setter 返回对应 builder 的引用。配置关闭 renegotiation、early data、post-handshake client authentication、应用 session cache 和主动 ticket 发行。TLS 客户端始终验证证书链与 SAN，禁止 CN fallback 和部分 label 通配符；没有关闭验证的便捷选项。

## 握手

```cpp
// stream 是已经连接、由当前运行时驱动的 TcpStream。
auto server_name = faio::tls::ServerName::parse("example.com");
if (!server_name) { /* 处理错误 */ }
auto encrypted = co_await connector.connect(std::move(*server_name), std::move(stream));
if (!encrypted) { /* 处理 encrypted.error() */ }
```

| 方法 | 返回类型和作用 |
| --- | --- |
| `TlsConnector(ClientConfig)` / `TlsAcceptor(ServerConfig)` | 只保存共享配置，不进行网络 I/O |
| 两者的 `config() const noexcept` | 返回只读配置副本，共享 SSL_CTX |
| `TlsConnector::with_alpn(vector<string>) const` | `expected<TlsConnector>`；派生独立 ALPN 列表，复用 SSL_CTX 和身份，不修改原 Connector |
| `connect(ServerName, S, OperationOptions={}) const` | `task<expected<TlsStream<S>>>`；验证链和 DNS/IP 身份、发送必要 SNI、完成握手并 flush 后交付 |
| `accept(S, OperationOptions={}) const` | 同上；执行服务端握手、mTLS 和 ALPN |

每个连接新建一个 SSL；S 原样保留，不作虚拟类型擦除。服务端按自己的 ALPN 列表顺序选择交集；双方提供了列表但无交集时失败，任一方没提供列表时允许协商结果为空。

所有异步公共函数在**调用时**保存 state、配置和输入值，首次 resume 不再借用原包装的 `this`。默认握手 deadline 是调用时间起 10 秒，显式更早的 deadline 优先。握手失败、取消和超时均终止传输，不交还半握手流。

## 字节流与半边

`TlsStream<S>` 满足 faio 的 borrowed/owned reader/writer concepts，可使用已有 `io::read_exact/write_all/copy`。返回类型沿用 `task<expected<T>>`，协议诊断保存在 `Error::context()`。

| 方法 | 成功和完成语义 |
| --- | --- |
| `read(span<char>, options={}) const` | `size_t`；一次短读。非空读取只有收到 close_notify 才返回 EOF 0，纯 TCP EOF 是 `unexpected_eof` |
| `read(io_buffer, options={}) const` | `io_transfer`；写入 capacity，成功后设置 size 为本次初始化字节数 |
| `write(span<const char>, options={}) const` | `size_t`；一次最多接受 16 KiB 明文，可仍缓存密文 |
| `write(io_buffer, options={}) const` | `io_transfer`；返还原 buffer 和本次接受量，不删去前缀 |
| `flush(options={}) const` | 排空先前接受的密文及控制输出，再调用下层 flush |
| `shutdown(options={}) const` | 排空并 flush 本地 close_notify；写半关闭，读方向仍可继续；重复成功幂等 |
| `close(options={}) const` | 默认 5 秒内完成双方通知和底层关闭；有未读应用数据则报 `unread_application_data` 并终止；成功后重复调用仍成功 |
| `abort() noexcept` | 非阻塞请求终止、停止在途 I/O；操作仍持有 state 到排空结束 |
| `is_terminated() const noexcept` | Failed/Closed/移动后为 true，单边关闭与 Closing 为 false |
| `connection_info() const` | 拥有型 `ConnectionInfo`：version、cipher、ALPN、verify_result、可选 peer leaf DER |
| `into_split() &&` | 消耗包装，返回一读一写两个拥有型半边；已有活跃用户操作时抛 `logic_error` 且不消费原包装 |

Stream 和 halves 可移动、不可复制，析构不阻塞。move 赋值先 abort 旧连接；普通 move 和 split 的转交不 abort 新所有者。ReadHalf 只提供两种 read、abort、终止和协商信息查询；WriteHalf 提供两种 write、flush、shutdown、close、abort 和同样查询。任一有效半边析构都会 abort 全连接，需要优雅关闭时先 await close。

同一连接最多一个用户 reader 和一个用户 writer。一读一写可并发；同向冲突立即 `EBUSY`。write/flush/shutdown 共用写方向；close 协调已有读写结束，后续 close 复用同一个关闭结果。

`ConnectionInfo::verify_result` 只在实际验证对端证书时有值。服务端未请求证书、optional 模式且没有客户端证书时为空；不能把默认 X509_V_OK 展示为已验证客户端身份。DER 和字符串均为拥有型副本，可在原连接销毁后继续使用。

## 取消、deadline 和错误

`faio::io::OperationOptions` 同时接受绝对 steady-clock deadline 与显式 stop token；父协程 token 与显式 token 取逻辑 OR。普通 read/write/flush 不自动增加超时。取消返回前会停止并排空本次内部 I/O，span 在此之前必须有效。

| 操作 | 取消后 |
| --- | --- |
| 进入 gate/SSL 写调用之前 | 返回取消，未接纳数据 |
| read | 保留密文、pending read 和内部明文；未赢得结果提交时不修改用户 span；可继续读 |
| write 已进入 SSL 调用 | 保守 abort；错误 progress 是已被 TLS 接受的明文字节 |
| flush/shutdown/close 已启动 | abort 并排空 |
| 握手 | 终止底层传输 |

TLS 不能直接使用只适配原生 I/O awaiter 的 `.set_timeout()`；使用 `OperationOptions`。`Error::progress()` 对 TLS read/write 以明文字节计，对握手/flush/shutdown/close 为 0。transport 错误保留 POSIX/Win32/Winsock/resolver domain；TLS 错误使用稳定 errc、拥有型 OpenSSL reason 列表和 verify_code。

`tls::errc` 的稳定值是 invalid_config=1、invalid_server_name=2、certificate_verify_failed=3、hostname_mismatch=4、handshake_failed=5、protocol_error=6、unexpected_eof=7、unread_application_data=8、closed=9、unsupported_backend=10。

## 示例与验证

构建 `tls_echo`。从仓库根目录运行时，传入测试证书、私钥和 CA：

```sh
./target/cmake-build-mac-main-release/examples/tls_echo tests/fixtures/server.pem tests/fixtures/server.key tests/fixtures/ca.pem
```

[TLS 回显程序](../../examples/tls/echo.cpp)在本机动态端口建立一次连接，验证服务证书及 `localhost` 名称，然后回显一条定长消息并完成 TLS 关闭。`tests/fixtures` 的私钥仅用于测试。
