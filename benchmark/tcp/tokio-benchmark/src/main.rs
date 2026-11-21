/**
 * @file benchmark/tcp/tokio-benchmark/src/main.rs
 * @brief Tokio TCP 回显性能对照。
 */
use std::env;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tokio::runtime::Builder;

/** @brief 处理 Tokio 连接的字节回显或 HTTP 持久响应。 */
async fn handle(mut stream: TcpStream, echo: bool) {
    stream.set_nodelay(true).unwrap();
    let mut buf = vec![0u8; 65536];
    let mut request_buf = Vec::<u8>::with_capacity(4096);
    let response = b"HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: 16\r\nConnection: keep-alive\r\n\r\nhello benchmark\n";

    loop {
        let n = match stream.read(&mut buf).await {
            Ok(0) => return,
            Ok(n) => n,
            Err(_) => return,
        };

        if echo {
            if stream.write_all(&buf[..n]).await.is_err() { return; }
            continue;
        }
        if request_buf.len() + n > 65536 { return; }
        request_buf.extend_from_slice(&buf[..n]);

        loop {
            let pos = request_buf.windows(4).position(|w| w == b"\r\n\r\n");
            let Some(end) = pos else { break; };

            if stream.write_all(response).await.is_err() {
                return;
            }
            request_buf.drain(..end + 4);
        }
    }
}

/** @brief 绑定基准端口并提交各连接处理任务。 */
async fn run_server() {
    let args: Vec<String> = env::args().collect();
    let host = args.get(1).map(String::as_str).unwrap_or("0.0.0.0");
    let port = args
        .get(2)
        .and_then(|p| p.parse::<u16>().ok())
        .unwrap_or(18083);

    let echo = args.get(4).is_some_and(|s| s == "echo");
    let addr = format!("{}:{}", host, port);
    let listener = TcpListener::bind(&addr)
        .await
        .expect("failed to bind tokio tcp benchmark");

    println!("tokio tcp benchmark listening on {}", addr);

    loop {
        let (stream, _) = match listener.accept().await {
            Ok(v) => v,
            Err(_) => continue,
        };
        tokio::spawn(handle(stream, echo));
    }
}

/** @brief 解析参数并运行 Tokio TCP 回显或 HTTP 基准服务。 */
fn main() {
    let mut args = env::args();
    let workers = args.nth(3).map(|s| s.parse::<usize>().unwrap()).unwrap_or(4);
    assert!(workers > 0);
    let current_thread = args.skip(1).any(|arg| arg == "--current-thread");
    let mut builder = if current_thread {
        assert_eq!(workers, 1, "--current-thread requires workers=1");
        Builder::new_current_thread()
    } else {
        let mut builder = Builder::new_multi_thread();
        builder.worker_threads(workers);
        builder
    };
    let runtime = builder.enable_all().build().unwrap();
    if current_thread {
        println!("tokio tcp benchmark runtime: current_thread");
    }
    runtime.block_on(run_server());
}
