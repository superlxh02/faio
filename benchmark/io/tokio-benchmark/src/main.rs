/**
 * @file benchmark/io/tokio-benchmark/src/main.rs
 * @brief Tokio 文件 I/O 与 CPU 任务性能对照。
 */
use std::{error::Error, path::Path, time::Instant};
use tokio::io::{AsyncReadExt, AsyncSeekExt, AsyncWriteExt};

type Result<T> = std::result::Result<T, Box<dyn Error + Send + Sync>>;

/** @brief 输出每次操作的耗时、吞吐量与工作量校验和。 */
fn report(scenario: &str, start: Instant, operations: usize, bytes: usize, checksum: u64) {
    let elapsed = start.elapsed().as_secs_f64();
    println!("{scenario},{:.3},{:.3},{:.3},{operations},{bytes},{checksum}",
        elapsed * 1e9 / operations as f64, operations as f64 / elapsed,
        bytes as f64 / elapsed / 1_048_576.0);
}

/** @brief 预热普通文件后测量指定块大小的完整读写。 */
async fn file_case(path: &Path, block_size: usize, operations: usize) -> Result<()> {
    let mut file = tokio::fs::OpenOptions::new().read(true).write(true).create(true)
        .truncate(true).open(path).await?;
    file.set_max_buf_size(2 * 1024 * 1024);
    let mut buffer = vec![b'f'; block_size];
    for _ in 0..operations { file.write_all(&buffer).await?; }
    file.sync_data().await?;
    file.seek(std::io::SeekFrom::Start(0)).await?;
    let start_write = Instant::now();
    for _ in 0..operations { file.write_all(&buffer).await?; }
    file.flush().await?;
    report(&format!("file_write_all_{block_size}"), start_write, operations, block_size * operations, 0);
    file.sync_data().await?;
    file.seek(std::io::SeekFrom::Start(0)).await?;
    buffer.fill(0);
    let start_read = Instant::now();
    let mut checksum = 0_u64;
    for _ in 0..operations {
        let count = file.read_exact(&mut buffer).await?;
        if count != block_size || buffer[0] != b'f' || buffer[block_size - 1] != b'f' {
            return Err("file data mismatch".into());
        }
        checksum += u64::from(buffer[0]);
    }
    report(&format!("file_read_exact_{block_size}"), start_read, operations,
        block_size * operations, checksum);
    drop(file);
    Ok(())
}

/** @brief 执行固定轮数的整数依赖链并返回校验值。 */
fn cpu_work(mut value: u64, rounds: usize) -> u64 {
    for _ in 0..rounds {
        value ^= value >> 12;
        value ^= value << 25;
        value ^= value >> 27;
        value = value.wrapping_mul(2_685_821_657_736_338_717);
    }
    value
}

/** @brief 比较普通协程与阻塞池提交 CPU 负载的开销。 */
async fn cpu_case(jobs: usize, rounds: usize, blocking: bool) -> Result<()> {
    let mut handles = Vec::with_capacity(jobs);
    let start = Instant::now();
    for i in 0..jobs {
        handles.push(if blocking {
            tokio::task::spawn_blocking(move || cpu_work(i as u64 + 1, rounds))
        } else {
            tokio::spawn(async move { cpu_work(i as u64 + 1, rounds) })
        });
    }
    let mut checksum = 0_u64;
    for handle in handles { checksum ^= handle.await?; }
    report(&format!("{}_{rounds}", if blocking {"cpu_spawn_blocking"} else {"cpu_spawn"}),
        start, jobs, 0, checksum);
    Ok(())
}

/** @brief 运行选定的文件 I/O 和 CPU 负载场景。 */
async fn suite(directory: &Path, n: usize, selection: &str) -> Result<()> {
    if selection != "cpu" {
        for block in [4096_usize, 65536, 8388608] {
            let count = (n * 4096 / block).max(8);
            let path = directory.join(format!("tokio-perf-{block}.data"));
            file_case(&path, block, count).await?;
            std::fs::remove_file(path)?;
        }
    }
    if selection != "io" {
        cpu_case(128, 20000, false).await?;
        cpu_case(128, 20000, true).await?;
        cpu_case(128, 200000, false).await?;
        cpu_case(128, 200000, true).await?;
    }
    Ok(())
}

/** @brief 运行 Tokio 普通文件及 CPU 基准，并输出 CSV。 */
fn main() -> Result<()> {
    let args: Vec<String> = std::env::args().collect();
    let directory = args.get(1).ok_or("usage: io_cpu_benchmark DIRECTORY [4k-operations=2048] [workers=4] [all|io|cpu]")?;
    let directory = Path::new(directory);
    if !directory.is_dir() { return Err("directory must already exist".into()); }
    let n = args.get(2).map(|s| s.parse::<usize>()).transpose()?.unwrap_or(2048);
    let workers = args.get(3).map(|s| s.parse::<usize>()).transpose()?.unwrap_or(4);
    let selection = args.get(4).map(String::as_str).unwrap_or("all");
    if n == 0 || workers == 0 || !["all", "io", "cpu"].contains(&selection) {
        return Err("invalid count, workers or suite".into());
    }
    let runtime = tokio::runtime::Builder::new_multi_thread().worker_threads(workers)
        .max_blocking_threads(workers).build()?;
    println!("scenario,ns_per_op,ops_per_sec,mib_per_sec,operations,bytes,checksum");
    runtime.block_on(suite(directory, n, selection))
}
