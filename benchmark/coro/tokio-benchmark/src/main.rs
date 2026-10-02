// 与 ../faio_coro_benchmark.cpp 对齐。Tokio 无原生 CV/latch，组合实现单独标注。
use std::{sync::{Arc, atomic::{AtomicUsize, Ordering}}, time::Instant};
use tokio::{runtime::{Builder, Runtime}, sync::{Semaphore, Mutex, mpsc, Notify}};

// 参数在 worker 启动前写入；之后不再修改。计时辅助函数不分配内存。
static SAMPLE_ENABLED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(true);
static WARMING: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
static SAMPLE_DIR: std::sync::OnceLock<std::path::PathBuf> = std::sync::OnceLock::new();
static TIME_BASE: std::sync::OnceLock<Instant> = std::sync::OnceLock::new();
fn sample_start() -> Option<Instant> { SAMPLE_ENABLED.load(Ordering::Relaxed).then(Instant::now) }
fn sample_elapsed(t: Option<Instant>) -> f64 { t.map(elapsed).unwrap_or(0.) }
fn timestamp_ns() -> u64 {
    if SAMPLE_ENABLED.load(Ordering::Relaxed) { TIME_BASE.get().unwrap().elapsed().as_nanos() as u64 } else { 0 }
}
struct Measurement { ns_per_op: f64, samples: Vec<f64>, migrations: usize }
fn elapsed(start: Instant) -> f64 { start.elapsed().as_nanos() as f64 }
fn report(name: &str, mut r: Measurement) {
    if WARMING.load(Ordering::Relaxed) { return; }
    if SAMPLE_ENABLED.load(Ordering::Relaxed) {
        if let Some(dir)=SAMPLE_DIR.get() {
            use std::io::Write;
            let mut out=std::io::BufWriter::new(std::fs::File::create(dir.join(format!("{name}.csv"))).unwrap());
            writeln!(out,"sample_index,latency_ns").unwrap();
            for (i,v) in r.samples.iter().enumerate(){writeln!(out,"{i},{v}").unwrap();}
        }
    }
    r.samples.sort_by(f64::total_cmp);
    let p = |q: f64| r.samples[((r.samples.len()-1) as f64*q) as usize];
    println!("{},{:.2},{:.2},{:.2},{:.2},{:.2},{:.2},{},{}",name,r.ns_per_op,p(0.5),p(0.9),p(0.99),p(0.999),r.samples.last().unwrap(),r.migrations,r.samples.len());
}
fn runtime(workers: usize) -> Runtime { Builder::new_multi_thread().worker_threads(workers).enable_all().build().unwrap() }
async fn one() -> u64 { 1 }
async fn ready_paths(n: usize, mode: u8) -> Measurement {
    let mut r=Measurement{ns_per_op:0.,samples:vec![0.;n],migrations:0};
    let sem=Semaphore::new(1);let mutex=Mutex::new(());let (tx,mut rx)=mpsc::channel::<u64>(64);
    let mut checksum=0;let start=Instant::now();
    for i in 0..n {
        let t=sample_start();
        match mode {
            0 => { let permit=sem.acquire().await.unwrap();drop(permit); }
            1 => { let guard=mutex.lock().await;drop(guard); }
            2 => {tx.send(i as u64).await.unwrap();checksum+=rx.recv().await.unwrap();}
            3 => {tx.try_send(i as u64).unwrap();checksum+=rx.try_recv().unwrap();}
            4 => {checksum+=one().await;}
            _ => unreachable!()
        }
        r.samples[i]=sample_elapsed(t);
    }
    r.ns_per_op=elapsed(start)/n as f64;
    if mode==2||mode==3 {assert_eq!(checksum,(n-1) as u64*n as u64/2);}
    if mode==4 {assert_eq!(checksum,n as u64);}
    r
}
async fn yields(n: usize) -> Measurement {
    let mut r=Measurement{ns_per_op:0.,samples:vec![0.;n],migrations:0};let start=Instant::now();
    for i in 0..n {
        let worker=std::thread::current().id();let t=sample_start();
        tokio::task::yield_now().await;r.samples[i]=sample_elapsed(t);
        r.migrations+=usize::from(worker!=std::thread::current().id());
    }
    r.ns_per_op=elapsed(start)/n as f64;r
}
async fn combinators(n: usize, mode: u8) -> Measurement {
    let mut r=Measurement{ns_per_op:0.,samples:vec![0.;n],migrations:0};let mut sum=0;let start=Instant::now();
    for i in 0..n {
        let t=sample_start();
        match mode {
            0 => {sum+=tokio::spawn(one()).await.unwrap();}
            // 对齐 faio join：两个分支是独立已提交任务，不是单任务内的 join!。
            1 => {let (a,b)=tokio::join!(tokio::spawn(one()),tokio::spawn(one()));sum+=a.unwrap()+b.unwrap();}
            // 对齐 faio select 的提交+排空成本，赢家返回后仍等待另一个已就绪分支结束。
            // 这里只测立即完成任务，没有把 Rust future drop 当作 C++ 协作取消。
            2 => {
                let mut a=tokio::spawn(one());let mut b=tokio::spawn(one());
                tokio::select! { v=&mut a => {sum+=v.unwrap();b.await.unwrap();}, v=&mut b => {sum+=v.unwrap();a.await.unwrap();} }
            }
            _ => unreachable!()
        }
        r.samples[i]=sample_elapsed(t);
    }
    assert_eq!(sum,n as u64*if mode==1 {2}else{1});r.ns_per_op=elapsed(start)/n as f64;r
}
async fn task_groups(n:usize, scoped:bool)->Measurement {
    let mut r=Measurement{ns_per_op:0.,samples:vec![0.;n],migrations:0};let start=Instant::now();
    for sample in &mut r.samples {
        let t=sample_start();
        if scoped {
            let mut group=tokio::task::JoinSet::new();
            for _ in 0..32 {group.spawn(async {one().await;});}
            let mut count=0;while let Some(value)=group.join_next().await {value.unwrap();count+=1;}assert_eq!(count,32);
        } else {
            let mut handles=Vec::with_capacity(32);for _ in 0..32 {handles.push(tokio::spawn(one()));}
            let mut values=Vec::with_capacity(32);for h in handles {values.push(h.await.unwrap());}
            assert_eq!(values.into_iter().sum::<u64>(),32);
        }
        *sample=sample_elapsed(t);
    }
    r.ns_per_op=elapsed(start)/n as f64;r
}
async fn barrier_ready(n:usize)->Measurement {
    let barrier=tokio::sync::Barrier::new(1);let mut r=Measurement{ns_per_op:0.,samples:vec![0.;n],migrations:0};let start=Instant::now();
    for sample in &mut r.samples {let t=sample_start();barrier.wait().await;*sample=sample_elapsed(t);}
    r.ns_per_op=elapsed(start)/n as f64;r
}
struct Completion { left: AtomicUsize, done: Notify }
async fn record(sent: Option<Instant>, index: usize, completion: Arc<Completion>, samples: Arc<Vec<std::sync::atomic::AtomicU64>>) {
    samples[index].store(sample_elapsed(sent) as u64,Ordering::Relaxed);
    if completion.left.fetch_sub(1,Ordering::AcqRel)==1 {completion.done.notify_one();}
}
fn burst(n: usize, workers: usize, internal: bool) -> Measurement {
    let rt=runtime(workers);let completion=Arc::new(Completion{left:AtomicUsize::new(n),done:Notify::new()});
    let samples=Arc::new((0..n).map(|_|std::sync::atomic::AtomicU64::new(0)).collect::<Vec<_>>());
    let start=Instant::now();
    if internal {
        let c=completion.clone();let s=samples.clone();
        rt.block_on(rt.spawn(async move {
            for i in 0..n {tokio::spawn(record(sample_start(),i,c.clone(),s.clone()));}
            c.done.notified().await;
        })).unwrap();
    } else {
        for i in 0..n {rt.spawn(record(sample_start(),i,completion.clone(),samples.clone()));}
        rt.block_on(completion.done.notified());
    }
    let ns_per_op=elapsed(start)/n as f64;
    Measurement{ns_per_op,samples:samples.iter().map(|v|v.load(Ordering::Relaxed) as f64).collect(),migrations:0}
}
struct Message { value:u64, sent_ns:u64 }
async fn producer(tx:mpsc::Sender<Message>,n:usize,offset:usize) {
    for i in 0..n {tx.send(Message{value:(i+offset) as u64,sent_ns:timestamp_ns()}).await.unwrap();}
}
async fn pipeline(n:usize,producers:usize,capacity:usize)->Measurement {
    let (tx,mut rx)=mpsc::channel(capacity);let mut r=Measurement{ns_per_op:0.,samples:vec![0.;n],migrations:0};
    let mut handles=Vec::with_capacity(producers);let start=Instant::now();
    for p in 0..producers {handles.push(tokio::spawn(producer(tx.clone(),n/producers,p*(n/producers))));}
    let mut sum=0;
    for sample in &mut r.samples {let m=rx.recv().await.unwrap();*sample=(timestamp_ns()-m.sent_ns) as f64;sum+=m.value;}
    for h in handles {h.await.unwrap();}
    assert_eq!(sum,(n-1) as u64*n as u64/2);r.ns_per_op=elapsed(start)/n as f64;r
}
async fn echo(request:Arc<Semaphore>,reply:Arc<Semaphore>,n:usize) {
    for _ in 0..n {request.acquire().await.unwrap().forget();reply.add_permits(1);}
}
async fn ping(request:Arc<Semaphore>,reply:Arc<Semaphore>,n:usize)->Measurement {
    let mut r=Measurement{ns_per_op:0.,samples:vec![0.;n],migrations:0};let start=Instant::now();
    for sample in &mut r.samples {let t=sample_start();request.add_permits(1);reply.acquire().await.unwrap().forget();*sample=sample_elapsed(t);}
    r.ns_per_op=elapsed(start)/n as f64;r
}
fn cross_runtime(n:usize)->Measurement {
    let a=runtime(1);let b=runtime(1);let request=Arc::new(Semaphore::new(0));let reply=Arc::new(Semaphore::new(0));
    let h=b.spawn(echo(request.clone(),reply.clone(),n));let r=a.block_on(a.spawn(ping(request,reply,n))).unwrap();
    b.block_on(h).unwrap();r
}
fn external_notification(n:usize)->Measurement {
    use std::{future::{poll_fn, Future}, sync::atomic::AtomicU64};
    let rt=runtime(1);let sem=Arc::new(Semaphore::new(0));let armed=Arc::new(AtomicUsize::new(0));let ack=Arc::new(AtomicUsize::new(0));
    let sent=Arc::new(AtomicU64::new(0));let base=Instant::now();
    let (s,a,k,stamp)=(sem.clone(),armed.clone(),ack.clone(),sent.clone());
    let h=rt.spawn(async move {
        let mut r=Measurement{ns_per_op:0.,samples:vec![0.;n],migrations:0};
        for i in 0..n {
            let mut acquire=std::pin::pin!(s.acquire());
            let permit=poll_fn(|cx| {
                let state=acquire.as_mut().poll(cx);
                if state.is_pending() {a.store(i+1,Ordering::Release);}
                state
            }).await.unwrap();permit.forget();
            r.samples[i]=if SAMPLE_ENABLED.load(Ordering::Relaxed) {(base.elapsed().as_nanos() as u64-stamp.load(Ordering::Relaxed)) as f64} else {0.};
            k.store(i+1,Ordering::Release);
        }
        r
    });let start=Instant::now();
    for i in 0..n {
        while armed.load(Ordering::Acquire)<=i {std::sync::atomic::compiler_fence(Ordering::SeqCst);}
        sent.store(if SAMPLE_ENABLED.load(Ordering::Relaxed){base.elapsed().as_nanos() as u64}else{0},Ordering::Relaxed);sem.add_permits(1);
        while ack.load(Ordering::Acquire)<=i {std::sync::atomic::compiler_fence(Ordering::SeqCst);}
    }
    let mut r=rt.block_on(h).unwrap();r.ns_per_op=elapsed(start)/n as f64;r
}
fn block_entry(n:usize)->Measurement {
    let rt=runtime(1);let mut r=Measurement{ns_per_op:0.,samples:vec![0.;n],migrations:0};let start=Instant::now();
    // spawn 到 worker 后 block_on 等待，避免把 host 线程上直接 poll 一个 Ready future
    // 与 faio 提交到 worker 后阻塞等待进行比较。
    for sample in &mut r.samples {let t=sample_start();assert_eq!(rt.block_on(rt.spawn(one())).unwrap(),1);*sample=sample_elapsed(t);}
    r.ns_per_op=elapsed(start)/n as f64;r
}

async fn same_thread_handoff(n:usize)->Measurement {
    let request=Arc::new(Semaphore::new(0));let reply=Arc::new(Semaphore::new(0));
    let h=tokio::spawn(echo(request.clone(),reply.clone(),n));let r=ping(request,reply,n).await;h.await.unwrap();r
}
async fn contention(n:usize,use_mutex:bool)->Measurement {
    let mutex=Arc::new(Mutex::new(()));let sem=Arc::new(Semaphore::new(2));
    let done=Arc::new(AtomicUsize::new(0));let mut handles=Vec::with_capacity(4);
    let buffers=(0..4).map(|_|vec![0.;n/4]).collect::<Vec<_>>();
    let mut chunks=Vec::with_capacity(4);let start=Instant::now();
    for mut samples in buffers {
        let (m,s,d)=(mutex.clone(),sem.clone(),done.clone());
        handles.push(tokio::spawn(async move {
            for sample in &mut samples {
                let t=sample_start();
                if use_mutex {let g=m.lock().await;tokio::task::yield_now().await;drop(g);}
                else {let p=s.acquire().await.unwrap();tokio::task::yield_now().await;drop(p);}
                d.fetch_add(1,Ordering::Relaxed);*sample=sample_elapsed(t);
            }
            samples
        }));
    }
    for h in handles {chunks.push(h.await.unwrap());}
    let ns_per_op=elapsed(start)/n as f64;assert_eq!(done.load(Ordering::Relaxed),n);
    let samples=chunks.into_iter().flatten().collect();
    Measurement{ns_per_op,samples,migrations:0}
}
async fn barrier_contended(n:usize)->Measurement {
    let barrier=Arc::new(tokio::sync::Barrier::new(4));let mut handles=Vec::with_capacity(4);
    let buffers=(0..4).map(|_|vec![0.;n/4]).collect::<Vec<_>>();let mut chunks=Vec::with_capacity(4);let start=Instant::now();
    for mut samples in buffers {let b=barrier.clone();handles.push(tokio::spawn(async move {
        for v in &mut samples {let t=sample_start();b.wait().await;*v=sample_elapsed(t);}samples
    }));}
    for h in handles {chunks.push(h.await.unwrap());}
    let ns_per_op=elapsed(start)/n as f64;let samples=chunks.into_iter().flatten().collect();
    Measurement{ns_per_op,samples,migrations:0}
}
// Notify 不是原生条件变量：登记通知后解锁、醒来后重新加锁检查谓词。
async fn wait_turn<'a>(mutex:&'a Mutex<u8>,notify:&Notify,want:u8)->tokio::sync::MutexGuard<'a,u8> {
    let mut guard=mutex.lock().await;
    while *guard!=want {
        let notified=notify.notified();tokio::pin!(notified);notified.as_mut().enable();
        drop(guard);notified.await;guard=mutex.lock().await;
    }
    guard
}
async fn cv_roundtrip(n:usize)->Measurement {
    let mutex=Arc::new(Mutex::new(0u8));let notify=Arc::new(Notify::new());let (m,c)=(mutex.clone(),notify.clone());
    let h=tokio::spawn(async move {for _ in 0..n {let mut g=wait_turn(&m,&c,1).await;*g=0;drop(g);c.notify_one();}});
    let mut r=Measurement{ns_per_op:0.,samples:vec![0.;n],migrations:0};let start=Instant::now();
    for v in &mut r.samples {
        let t=sample_start();let mut g=mutex.lock().await;*g=1;notify.notify_one();drop(g);
        let g=wait_turn(&mutex,&notify,0).await;drop(g);*v=sample_elapsed(t);
    }
    h.await.unwrap();r.ns_per_op=elapsed(start)/n as f64;r
}
async fn latch_groups(n:usize,ready:bool)->Measurement {
    let opened=Completion{left:AtomicUsize::new(0),done:Notify::new()};
    let mut r=Measurement{ns_per_op:0.,samples:vec![0.;n],migrations:0};let start=Instant::now();
    for v in &mut r.samples {
        let t=sample_start();
        if ready {assert_eq!(opened.left.load(Ordering::Acquire),0);}
        else {
            let latch=Arc::new(Completion{left:AtomicUsize::new(32),done:Notify::new()});let mut handles=Vec::with_capacity(32);
            for _ in 0..32 {let l=latch.clone();handles.push(tokio::spawn(async move {if l.left.fetch_sub(1,Ordering::AcqRel)==1{l.done.notify_one();}}));}
            if latch.left.load(Ordering::Acquire)!=0 {latch.done.notified().await;}
            for h in handles {h.await.unwrap();}assert_eq!(latch.left.load(Ordering::Acquire),0);
        }
        *v=sample_elapsed(t);
    }
    r.ns_per_op=elapsed(start)/n as f64;r
}
fn timer_calibration(n:usize)->Measurement {
    let mut r=Measurement{ns_per_op:0.,samples:vec![0.;n],migrations:0};let start=Instant::now();
    for v in &mut r.samples{let t=sample_start();*v=sample_elapsed(t);}
    r.ns_per_op=elapsed(start)/n as f64;r
}

fn main() {
    let n=std::env::args().nth(1).map(|s|s.parse::<usize>().unwrap()).unwrap_or(100000)/4*4;assert!(n>=32);
    let args=std::env::args().collect::<Vec<_>>();
    SAMPLE_ENABLED.store(args.get(2).map(|s|s!="batch").unwrap_or(true),Ordering::Relaxed);
    if let Some(dir)=args.get(3){std::fs::create_dir_all(dir).unwrap();SAMPLE_DIR.set(dir.into()).unwrap();}
    TIME_BASE.set(Instant::now()).unwrap();assert_eq!(std::mem::size_of::<Message>(),16);
    println!("scenario,ns_per_op,p50_ns,p90_ns,p99_ns,p999_ns,max_ns,migrations,operations");
    let requested_n=n;
    for warming in [true,false] {
    WARMING.store(warming,Ordering::Relaxed);let n=if warming {requested_n.min(1024)}else{requested_n};
    report("timer_calibration",timer_calibration(n));
    for workers in [1,4] {
        let rt=runtime(workers);rt.block_on(rt.spawn(yields(1000))).unwrap();
        report(&format!("yield_{workers}"),rt.block_on(rt.spawn(yields(n))).unwrap());
        report(&format!("handoff_rtt_w{workers}"),rt.block_on(rt.spawn(same_thread_handoff(n.min(10000)))).unwrap());
        report(&format!("mutex_contention_p4_w{workers}"),rt.block_on(rt.spawn(contention(n,true))).unwrap());
        report(&format!("semaphore_contention_k2_p4_w{workers}"),rt.block_on(rt.spawn(contention(n,false))).unwrap());
        report(&format!("barrier_p4_w{workers}"),rt.block_on(rt.spawn(barrier_contended(n))).unwrap());
        report(&format!("cv_roundtrip_w{workers}"),rt.block_on(rt.spawn(cv_roundtrip(n.min(10000)))).unwrap());
        report(&format!("latch_fanin_32_w{workers}"),rt.block_on(rt.spawn(latch_groups(n/32,false))).unwrap());
        report(&format!("spawn_join_{workers}"),rt.block_on(rt.spawn(combinators(n,0))).unwrap());
        if workers==1 {
            for (mode,name) in ["semaphore_ready","mutex_ready","mpsc_ready_64","mpsc_try_64","task_await_ready"].iter().enumerate() {
                report(name,rt.block_on(rt.spawn(ready_paths(n,mode as u8))).unwrap());
            }
            report("latch_ready",rt.block_on(rt.spawn(latch_groups(n,true))).unwrap());
            report("barrier_ready_1",rt.block_on(rt.spawn(barrier_ready(n))).unwrap());
            report("join_all_32",rt.block_on(rt.spawn(task_groups(n/32,false))).unwrap());
            report("scope_32",rt.block_on(rt.spawn(task_groups(n/32,true))).unwrap());
            report("join_ready_2",rt.block_on(rt.spawn(combinators(n,1))).unwrap());
            report("select_spawn_drain_2",rt.block_on(rt.spawn(combinators(n,2))).unwrap());
        }
        for capacity in [64,1024] {for producers in [1,4] {
            report(&format!("mpsc_p{producers}_w{workers}_c{capacity}"),rt.block_on(rt.spawn(pipeline(n,producers,capacity))).unwrap());
        }}
        report(&format!("external_burst_{workers}"),burst(n,workers,false));
        report(&format!("internal_burst_{workers}"),burst(n,workers,true));
    }
    report("cross_runtime_rtt",cross_runtime(n.min(10000)));
    report("external_notification_registered",external_notification(n.min(10000)));
    report("block_on_entry",block_entry(n.min(10000)));
    }
}
