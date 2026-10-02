use std::env;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};

async fn handle(mut stream: TcpStream) {
    let mut buf = vec![0u8; 8192];
    let mut request_buf = Vec::<u8>::with_capacity(4096);
    let response = b"HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: 16\r\nConnection: keep-alive\r\n\r\nhello benchmark\n";

    loop {
        let n = match stream.read(&mut buf).await {
            Ok(0) => return,
            Ok(n) => n,
            Err(_) => return,
        };

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

async fn run_server() {
    let args: Vec<String> = env::args().collect();
    let host = args.get(1).map(String::as_str).unwrap_or("0.0.0.0");
    let port = args
        .get(2)
        .and_then(|p| p.parse::<u16>().ok())
        .unwrap_or(18083);

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
        tokio::spawn(handle(stream));
    }
}

// 与 C++ 服务统一 worker 数，参数为 host、port、workers。
fn main() {
    let workers=env::args().nth(3).map(|s|s.parse::<usize>().unwrap()).unwrap_or(4);
    assert!(workers>0);
    tokio::runtime::Builder::new_multi_thread().worker_threads(workers).enable_all().build().unwrap().block_on(run_server());
}
