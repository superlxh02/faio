use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};
use tokio::net::UnixStream;
use tokio::runtime::{Builder, Runtime};

struct Measurement { cost: f64, samples: Vec<f64> }
fn elapsed(t: Instant) -> f64 { t.elapsed().as_nanos() as f64 }
fn report(name: &str, mut r: Measurement) {
    r.samples.sort_by(f64::total_cmp);
    let p = |q: f64| r.samples[((r.samples.len()-1) as f64*q) as usize];
    println!("{name},{:.2},{:.2},{:.2},{:.2},{:.2},{}", r.cost,p(0.5),p(0.99),p(0.999),r.samples.last().unwrap(),r.samples.len());
}
async fn one() -> i32 { 42 }
async fn coro(n: usize, kind: u8, sampling: bool) -> Measurement {
    let mut r = Measurement { cost: 0., samples: vec![0.;n] };
    let start = Instant::now();
    for s in &mut r.samples {
        let t = sampling.then(Instant::now);
        match kind {
            0 => tokio::task::yield_now().await,
            1 => assert_eq!(one().await,42),
            _ => assert_eq!(tokio::spawn(one()).await.unwrap(),42),
        }
        if let Some(t) = t { *s = elapsed(t); }
    }
    r.cost = elapsed(start)/n as f64; r
}
async fn blocking_serial(n: usize, sampling: bool) -> Measurement {
    let mut r = Measurement { cost: 0., samples: vec![0.;n] };
    let start = Instant::now();
    for s in &mut r.samples {
        let t = sampling.then(Instant::now);
        assert_eq!(tokio::task::spawn_blocking(||42).await.unwrap(),42);
        if let Some(t) = t { *s = elapsed(t); }
    }
    r.cost = elapsed(start)/n as f64; r
}
async fn blocking_burst(n: usize, sampling: bool) -> Measurement {
    let mut handles = Vec::with_capacity(n);
    let start = Instant::now();
    for _ in 0..n {
        let t = sampling.then(Instant::now);
        handles.push(tokio::task::spawn_blocking(move || t.map(elapsed).unwrap_or(0.)));
    }
    let mut samples = Vec::with_capacity(n);
    for h in handles { samples.push(h.await.unwrap()); }
    Measurement { cost: elapsed(start)/n as f64, samples }
}
async fn send(s: &UnixStream, byte: u8) {
    loop {
        s.writable().await.unwrap();
        match s.try_write(&[byte]) {
            Ok(1) => return,
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => continue,
            other => panic!("send: {other:?}"),
        }
    }
}
async fn recv(s: &UnixStream) -> u8 {
    let mut byte = [0];
    loop {
        s.readable().await.unwrap();
        match s.try_read(&mut byte) {
            Ok(1) => return byte[0],
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => continue,
            other => panic!("recv: {other:?}"),
        }
    }
}
async fn io_roundtrip(n: usize, sampling: bool) -> Measurement {
    let (a,b) = UnixStream::pair().unwrap();
    let peer = tokio::spawn(async move { for _ in 0..n { let byte=recv(&b).await;send(&b,byte).await; } });
    let mut r = Measurement { cost: 0., samples: vec![0.;n] };
    let start = Instant::now();
    for s in &mut r.samples {
        let t = sampling.then(Instant::now);
        send(&a,b'x').await; assert_eq!(recv(&a).await,b'x');
        if let Some(t) = t { *s=elapsed(t); }
    }
    peer.await.unwrap();r.cost=elapsed(start)/n as f64;r
}
async fn timer_busy(n: usize, sampling: bool) -> Measurement {
    let done = Arc::new(AtomicBool::new(false));
    let child = done.clone();
    let worker = tokio::spawn(async move { while !child.load(Ordering::Relaxed) { tokio::task::yield_now().await; } });
    let mut r = Measurement { cost: 0., samples: vec![0.;n] };
    let start = Instant::now();
    for s in &mut r.samples {
        let deadline = Instant::now()+Duration::from_millis(1);
        tokio::time::sleep_until(deadline.into()).await;
        if sampling { *s = elapsed(deadline); }
    }
    done.store(true,Ordering::Relaxed);worker.await.unwrap();r.cost=elapsed(start)/n as f64;r
}
fn external_burst(rt: &Runtime,n: usize) -> Measurement {
    let mut handles = Vec::with_capacity(n);
    let start = Instant::now();
    for _ in 0..n { handles.push(rt.spawn(one())); }
    rt.block_on(async { for h in handles { assert_eq!(h.await.unwrap(),42); } });
    Measurement { cost: elapsed(start)/n as f64, samples: vec![0.;n] }
}
fn main() {
    let args: Vec<_> = std::env::args().collect();
    let n: usize = args.get(1).map(|s| s.parse().unwrap()).unwrap_or(20000);
    assert!(n>=32);
    let sampling = args.get(2).is_none_or(|s|s!="batch");
    let multi = args.get(3).is_some_and(|s|s=="multi");
    let mut builder = if multi { Builder::new_multi_thread() } else { Builder::new_current_thread() };
    let rt = builder.worker_threads(1).max_blocking_threads(4).enable_all().build().unwrap();
    println!("scenario,ns_per_op,p50_ns,p99_ns,p999_ns,max_ns,operations");
    let mut calibration = Measurement { cost: 0., samples: vec![0.;n] };
    let calibration_start = Instant::now();
    for s in &mut calibration.samples { let t = Instant::now(); *s=elapsed(t); }
    calibration.cost=elapsed(calibration_start)/n as f64;report("clock_two_reads",calibration);
    report("blocking_cold",rt.block_on(blocking_serial(1,sampling)));
    rt.block_on(coro(1000,0,sampling));rt.block_on(blocking_serial(128,sampling));
    for kind in 0..3 { report(["yield","task_await","spawn_join"][kind as usize],rt.block_on(rt.spawn(coro(n,kind,sampling))).unwrap()); }
    let mut r = Measurement { cost: 0., samples: vec![0.;100] };
    let start=Instant::now();
    for s in &mut r.samples { let t=sampling.then(Instant::now);assert_eq!(rt.block_on(one()),42);if let Some(t)=t {*s=elapsed(t);} }
    r.cost=elapsed(start)/r.samples.len() as f64;report("block_on_entry",r);
    report("external_burst",external_burst(&rt,n));
    report("blocking_warm",rt.block_on(blocking_serial(n.min(2000),sampling)));
    report("blocking_burst_enqueue_to_start",rt.block_on(blocking_burst(n,sampling)));
    report("socketpair_rtt",rt.block_on(io_roundtrip(n.min(2000),sampling)));
    report("timer_1ms_busy_lateness",rt.block_on(timer_busy(32,sampling)));
}
