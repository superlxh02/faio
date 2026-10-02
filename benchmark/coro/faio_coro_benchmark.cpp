// 与本目录 tokio-benchmark 配对；场景语义与计时边界见 README.md。
#include "faio/faio.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using clock_type = std::chrono::steady_clock;
// 仅在创建 worker 前设置，此后所有线程只读；批量模式关闭逐操作读时钟。
bool sample_enabled = true;
bool warming = false;
std::filesystem::path sample_directory;
clock_type::time_point sample_start() { return sample_enabled ? clock_type::now() : clock_type::time_point{}; }
double sample_elapsed(clock_type::time_point t) {
  return sample_enabled ? std::chrono::duration<double, std::nano>(clock_type::now()-t).count() : 0.;
}
struct measurement {
  double ns_per_op{}; // 整段耗时/操作数，包含逐次采样开销。
  std::vector<double> samples; // 已预分配，每条样本写入后不再修改。
  std::size_t migrations{}; // yield 前后 worker 编号变化的次数；不代表 CPU 核迁移。
};
double elapsed(clock_type::time_point start) {
  return std::chrono::duration<double, std::nano>(clock_type::now() - start).count();
}
void report(const std::string& name, measurement result) {
  if(warming) return;
  if (sample_enabled && !sample_directory.empty()) {
    // 保留采样次序，CSV 输出在计时结束之后；Python 脚本压缩归档。
    std::ofstream out(sample_directory / (name + ".csv"));
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out << std::setprecision(17) << "sample_index,latency_ns\n";
    for (std::size_t i=0;i<result.samples.size();++i) out << i << ',' << result.samples[i] << '\n';
  }
  std::sort(result.samples.begin(), result.samples.end());
  auto p = [&](double q) { return result.samples[static_cast<std::size_t>((result.samples.size()-1)*q)]; };
  std::cout << name << ',' << std::fixed << std::setprecision(2) << result.ns_per_op
            << ',' << p(.5) << ',' << p(.9) << ',' << p(.99) << ',' << p(.999)
            << ',' << result.samples.back() << ',' << result.migrations << ',' << result.samples.size() << '\n';
}
faio::runtime::Config config(std::size_t workers) { return faio::ConfigBuilder{}.set_num_workers(workers).build(); }
faio::task<std::uint64_t> one() { co_return 1; }
faio::task<measurement> ready_paths(std::size_t n, int mode) {
  measurement r{0, std::vector<double>(n)};
  faio::sync::semaphore sem{1};
  faio::sync::mutex mutex;
  auto endpoints = faio::sync::mpsc<std::uint64_t>::make(64);
  auto& tx=endpoints.first;auto& rx=endpoints.second;
  std::uint64_t checksum=0;
  auto start=clock_type::now();
  for(std::size_t i=0;i<n;++i) {
    auto t=sample_start();
    if(mode==0) { co_await sem.acquire(); sem.release(); }
    if(mode==1) { co_await mutex.lock(); mutex.unlock(); }
    if(mode==2) { if(!(co_await tx.send(i))) throw std::runtime_error("send"); auto v=co_await rx.recv(); checksum+=*v; }
    if(mode==3) { if(!tx.try_send(i).value()) throw std::runtime_error("try_send"); checksum+=rx.try_recv().value().value(); }
    if(mode==4) checksum+=co_await one();
    r.samples[i]=sample_elapsed(t);
  }
  r.ns_per_op=elapsed(start)/n;
  if((mode==2||mode==3)&&checksum!=(n-1)*n/2) throw std::runtime_error("checksum");
  if(mode==4&&checksum!=n) throw std::runtime_error("checksum");
  co_return r;
}
faio::task<measurement> yields(std::size_t n) {
  measurement r{0,std::vector<double>(n)}; auto start=clock_type::now();
  for(std::size_t i=0;i<n;++i) {
    const auto worker=co_await faio::this_coro::worker_id(); auto t=sample_start();
    co_await faio::this_coro::yield(); r.samples[i]=sample_elapsed(t);
    r.migrations+=(co_await faio::this_coro::worker_id())!=worker;
  }
  r.ns_per_op=elapsed(start)/n; co_return r;
}
faio::task<measurement> combinators(std::size_t n,int mode) {
  measurement r{0,std::vector<double>(n)}; std::uint64_t sum=0; auto start=clock_type::now();
  for(std::size_t i=0;i<n;++i) {
    auto t=sample_start();
    if(mode==0) { auto h=faio::spawn(one()); sum+=co_await h; }
    if(mode==1) { auto [a,b]=co_await faio::join(one(),one()); sum+=a+b; }
    if(mode==2) { auto v=co_await faio::select(one(),one()); sum+=std::visit([](auto x){return x;},v.value); }
    r.samples[i]=sample_elapsed(t);
  }
  if(sum!=n*(mode==1?2:1)) throw std::runtime_error("checksum");
  r.ns_per_op=elapsed(start)/n; co_return r;
}
// 分组接口按每组 32 个 task 计时，n 是组数；与逐消息指标不能直接混算。
faio::task<measurement> task_groups(std::size_t n,bool scoped) {
  measurement r{0,std::vector<double>(n)};auto start=clock_type::now();
  for(auto& sample:r.samples) {
    auto t=sample_start();
    if(scoped) {
      co_await faio::scope([](faio::scope_context& group)->faio::task<void> {
        for(int i=0;i<32;++i) group.spawn(one());
        co_return;
      });
    } else {
      std::vector<faio::task<std::uint64_t>> tasks;tasks.reserve(32);
      for(int i=0;i<32;++i)tasks.push_back(one());
      auto values=co_await faio::join_all(std::move(tasks));
      std::uint64_t sum=0;for(auto value:values)sum+=value;if(sum!=32)throw std::runtime_error("group checksum");
    }
    sample=sample_elapsed(t);
  }
  r.ns_per_op=elapsed(start)/n;co_return r;
}
faio::task<measurement> barrier_ready(std::size_t n) {
  faio::sync::barrier barrier{1};measurement r{0,std::vector<double>(n)};auto start=clock_type::now();
  for(auto& sample:r.samples){auto t=sample_start();co_await barrier.arrive_and_wait();sample=sample_elapsed(t);}
  r.ns_per_op=elapsed(start)/n;co_return r;
}
faio::task<void> record(clock_type::time_point sent,double& output,std::atomic<std::size_t>& left) {
  output=sample_elapsed(sent);
  if(left.fetch_sub(1,std::memory_order_acq_rel)==1) left.notify_one();
  co_return;
}
faio::task<void> internal_submit(measurement& r,std::atomic<std::size_t>& left) {
  for(auto& sample:r.samples) faio::spawn_detached(record(sample_start(),sample,left));
  co_return;
}
measurement burst(std::size_t n,std::size_t workers,bool internal) {
  faio::runtime_context ctx{config(workers)}; measurement r{0,std::vector<double>(n)};
  std::atomic<std::size_t> left{n}; auto start=clock_type::now();
  if(internal) faio::block_on(ctx,internal_submit(r,left));
  else {
    for(auto& sample:r.samples) faio::spawn_detached(ctx,record(sample_start(),sample,left));
    for(auto seen=left.load(std::memory_order_acquire);seen;seen=left.load(std::memory_order_acquire)) left.wait(seen);
  }
  r.ns_per_op=elapsed(start)/n; return r; // runtime 关闭不计入新横向测试。
}
// 两边消息均为 16 字节：8 字节负载 + 8 字节时间戳；Rust Instant 本身可能为 16 字节。
struct message { std::uint64_t value; std::int64_t sent_ns; };
static_assert(sizeof(message)==16);
std::int64_t timestamp_ns() {
  return sample_enabled ? std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now().time_since_epoch()).count() : 0;
}
using channel=faio::sync::mpsc<message>;
faio::task<void> producer(channel::sender tx,std::size_t n,std::size_t offset) {
  for(std::size_t i=0;i<n;++i) if(!(co_await tx.send({i+offset,timestamp_ns()}))) throw std::runtime_error("closed");
}
faio::task<measurement> pipeline(std::size_t n,std::size_t producers,std::size_t capacity) {
  auto endpoints=channel::make(capacity);auto& tx=endpoints.first;auto& rx=endpoints.second; measurement r{0,std::vector<double>(n)};
  std::vector<faio::join_handle<void>> handles; handles.reserve(producers);
  const auto start=clock_type::now();
  for(std::size_t p=0;p<producers;++p) handles.push_back(faio::spawn(producer(tx,n/producers,p*(n/producers))));
  std::uint64_t sum=0;
  for(auto& sample:r.samples) { auto m=co_await rx.recv(); if(!m) throw std::runtime_error("closed"); sample=static_cast<double>(timestamp_ns()-m->sent_ns);sum+=m->value; }
  for(auto& h:handles) co_await h;
  if(sum!=(n-1)*n/2) throw std::runtime_error("checksum");
  r.ns_per_op=elapsed(start)/n;co_return r;
}
// 两个独立的单 worker runtime 保证两个协程恢复目标位于不同线程。
// 这是两次信号量交接的 RTT；不能把 RTT/2 冒充单向实测分位数。
faio::task<void> echo(faio::sync::semaphore& request,faio::sync::semaphore& reply,std::size_t n) {
  for(std::size_t i=0;i<n;++i) { co_await request.acquire();reply.release(); }
}
faio::task<measurement> ping(faio::sync::semaphore& request,faio::sync::semaphore& reply,std::size_t n) {
  measurement r{0,std::vector<double>(n)};auto start=clock_type::now();
  for(std::size_t i=0;i<n;++i) { auto t=sample_start();request.release();co_await reply.acquire();r.samples[i]=sample_elapsed(t); }
  r.ns_per_op=elapsed(start)/n;co_return r;
}
measurement cross_runtime(std::size_t n) {
  faio::runtime_context a{config(1)},b{config(1)};faio::sync::semaphore request{0},reply{0};
  auto h=faio::spawn(b,echo(request,reply,n));auto r=faio::block_on(a,ping(request,reply,n));h.get();return r;
}
// 登记完成才让外部线程发送：排除“预先已有 permit”的 acquire 快路径。
struct observed_acquire {
  faio::sync::semaphore::acquire_awaiter inner;
  std::atomic<std::size_t>& armed; // 由等待协程发布，由外部线程读取。
  std::size_t sequence; // 本次登记的编号。
  bool await_ready(){return inner.await_ready();}
  bool await_suspend(std::coroutine_handle<> h) {
    auto* flag=&armed;const auto number=sequence;const auto suspended=inner.await_suspend(h);
    flag->store(number,std::memory_order_release);return suspended; // 发布后不再访问帧内成员。
  }
  void await_resume(){inner.await_resume();}
};
faio::task<measurement> notification_receiver(faio::sync::semaphore& sem,std::atomic<std::size_t>& armed,
                                             std::atomic<std::size_t>& ack,const clock_type::time_point& sent,std::size_t n) {
  measurement r{0,std::vector<double>(n)};
  for(std::size_t i=0;i<n;++i) {
    co_await observed_acquire{sem.acquire(),armed,i+1};r.samples[i]=sample_elapsed(sent);
    ack.store(i+1,std::memory_order_release);
  }
  co_return r;
}
measurement external_notification(std::size_t n) {
  faio::runtime_context ctx{config(1)};faio::sync::semaphore sem{0};
  std::atomic<std::size_t> armed{0},ack{0};clock_type::time_point sent;
  auto handle=faio::spawn(ctx,notification_receiver(sem,armed,ack,sent,n));auto start=clock_type::now();
  for(std::size_t i=0;i<n;++i) {
    while(armed.load(std::memory_order_acquire)<=i)std::atomic_signal_fence(std::memory_order_seq_cst);
    sent=sample_start();sem.release();
    while(ack.load(std::memory_order_acquire)<=i)std::atomic_signal_fence(std::memory_order_seq_cst);
  }
  auto r=handle.get();r.ns_per_op=elapsed(start)/n;return r;
}
measurement block_entry(std::size_t n) {
  faio::runtime_context ctx{config(1)};measurement r{0,std::vector<double>(n)};auto start=clock_type::now();
  for(auto& v:r.samples) {auto t=sample_start();if(faio::block_on(ctx,one())!=1)throw std::runtime_error("result");v=sample_elapsed(t);}
  r.ns_per_op=elapsed(start)/n;return r;
}

// 单 worker 上两条任务交替交接许可，确保测到两条协程之间的切换。
faio::task<measurement> same_thread_handoff(std::size_t n) {
  faio::sync::semaphore request{0},reply{0};
  auto handle=faio::spawn(echo(request,reply,n));
  auto r=co_await ping(request,reply,n); co_await handle; co_return r;
}
// 争用测试刻意在持锁/持许可期间 yield，让四个等待者形成队列。
// samples 是完整操作的延迟；ns_per_op 是整段墙钟时间/n，两者不应混称。
faio::task<void> contention_child(faio::sync::mutex& mutex,faio::sync::semaphore& sem,
    measurement& r,std::atomic<std::size_t>& done,std::size_t begin,std::size_t n,bool use_mutex) {
  for(std::size_t i=begin;i<begin+n;++i) {
    auto t=sample_start();
    if(use_mutex) co_await mutex.lock(); else co_await sem.acquire();
    co_await faio::this_coro::yield();
    if(use_mutex) mutex.unlock(); else sem.release();
    done.fetch_add(1,std::memory_order_relaxed); r.samples[i]=sample_elapsed(t);
  }
}
faio::task<measurement> contention(std::size_t n,bool use_mutex) {
  faio::sync::mutex mutex; faio::sync::semaphore sem{2};
  measurement r{0,std::vector<double>(n)}; std::atomic<std::size_t> done{0};
  std::vector<faio::join_handle<void>> handles;handles.reserve(4);auto start=clock_type::now();
  for(std::size_t i=0;i<4;++i) handles.push_back(faio::spawn(contention_child(mutex,sem,r,done,i*(n/4),n/4,use_mutex)));
  for(auto& h:handles)co_await h;
  r.ns_per_op=elapsed(start)/n;if(done.load()!=n)throw std::runtime_error("contention count");co_return r;
}
faio::task<void> barrier_child(faio::sync::barrier& barrier,measurement& r,std::size_t begin,std::size_t n) {
  for(std::size_t i=begin;i<begin+n;++i){auto t=sample_start();co_await barrier.arrive_and_wait();r.samples[i]=sample_elapsed(t);}
}
faio::task<measurement> barrier_contended(std::size_t n) {
  faio::sync::barrier barrier{4};measurement r{0,std::vector<double>(n)};std::vector<faio::join_handle<void>> handles;
  handles.reserve(4);auto start=clock_type::now();
  for(std::size_t i=0;i<4;++i)handles.push_back(faio::spawn(barrier_child(barrier,r,i*(n/4),n/4)));
  for(auto& h:handles) { co_await h; }
  r.ns_per_op=elapsed(start)/n;co_return r;
}
faio::task<void> cv_echo(faio::sync::mutex& mutex,faio::sync::condition_variable& cv,int& turn,std::size_t n) {
  for(std::size_t i=0;i<n;++i) {
    co_await mutex.lock();co_await cv.wait(mutex,[&]{return turn==1;});
    turn=0;mutex.unlock();cv.notify_one();
  }
}
faio::task<measurement> cv_roundtrip(std::size_t n) {
  faio::sync::mutex mutex;faio::sync::condition_variable cv;int turn=0;
  auto h=faio::spawn(cv_echo(mutex,cv,turn,n));measurement r{0,std::vector<double>(n)};auto start=clock_type::now();
  for(auto& v:r.samples) {
    auto t=sample_start();co_await mutex.lock();turn=1;cv.notify_one();
    co_await cv.wait(mutex,[&]{return turn==0;});mutex.unlock();v=sample_elapsed(t);
  }
  co_await h;r.ns_per_op=elapsed(start)/n;co_return r;
}
faio::task<void> latch_child(faio::sync::latch& latch){latch.count_down();co_return;}
faio::task<measurement> latch_groups(std::size_t n,bool ready) {
  measurement r{0,std::vector<double>(n)};auto start=clock_type::now();faio::sync::latch opened{0};
  for(auto& v:r.samples) {
    auto t=sample_start();
    if(ready) co_await opened.wait();
    else {
      // 排空任务句柄，保证所有 count_down 的函数栈退出后再析构本组 latch。
      faio::sync::latch latch{32};std::vector<faio::join_handle<void>> handles;handles.reserve(32);
      for(int i=0;i<32;++i)handles.push_back(faio::spawn(latch_child(latch)));
      co_await latch.wait();for(auto& h:handles)co_await h;
    }
    v=sample_elapsed(t);
  }
  r.ns_per_op=elapsed(start)/n;co_return r;
}
measurement timer_calibration(std::size_t n) {
  measurement r{0,std::vector<double>(n)};auto start=clock_type::now();
  for(auto& v:r.samples){auto t=sample_start();v=sample_elapsed(t);}
  r.ns_per_op=elapsed(start)/n;return r;
}

} // namespace
int main(int argc,char**argv) {
  auto n=argc>1?std::stoull(argv[1]):100000; n=(n/4)*4;if(n<32)throw std::invalid_argument("count >= 32");
  sample_enabled=argc<3 || std::string(argv[2])!="batch";
  if(argc>3){sample_directory=argv[3];std::filesystem::create_directories(sample_directory);}
  std::cout<<"scenario,ns_per_op,p50_ns,p90_ns,p99_ns,p999_ns,max_ns,migrations,operations\n";
  const auto requested_n=n;
  for(bool warm:{true,false}) {
  warming=warm;n=warm?std::min(requested_n,1024ull):requested_n;
  report("timer_calibration",timer_calibration(n));
  for(auto workers:{1uz,4uz}) {
    faio::runtime_context ctx{config(workers)};
    // 不计时的预热，排除首次恢复/分配器初始化。
    (void)faio::block_on(ctx,yields(1000));
    report("yield_"+std::to_string(workers),faio::block_on(ctx,yields(n)));
    report("handoff_rtt_w"+std::to_string(workers),faio::block_on(ctx,same_thread_handoff(std::min(n,10000ull))));
    report("mutex_contention_p4_w"+std::to_string(workers),faio::block_on(ctx,contention(n,true)));
    report("semaphore_contention_k2_p4_w"+std::to_string(workers),faio::block_on(ctx,contention(n,false)));
    report("barrier_p4_w"+std::to_string(workers),faio::block_on(ctx,barrier_contended(n)));
    report("cv_roundtrip_w"+std::to_string(workers),faio::block_on(ctx,cv_roundtrip(std::min(n,10000ull))));
    report("latch_fanin_32_w"+std::to_string(workers),faio::block_on(ctx,latch_groups(n/32,false)));
    report("spawn_join_"+std::to_string(workers),faio::block_on(ctx,combinators(n,0)));
    if(workers==1) {
      const char* names[]={"semaphore_ready","mutex_ready","mpsc_ready_64","mpsc_try_64","task_await_ready"};
      for(int mode=0;mode<5;++mode) report(names[mode],faio::block_on(ctx,ready_paths(n,mode)));
      report("latch_ready",faio::block_on(ctx,latch_groups(n,true)));
      report("barrier_ready_1",faio::block_on(ctx,barrier_ready(n)));
      report("join_all_32",faio::block_on(ctx,task_groups(n/32,false)));
      report("scope_32",faio::block_on(ctx,task_groups(n/32,true)));
      report("join_ready_2",faio::block_on(ctx,combinators(n,1)));
      report("select_spawn_drain_2",faio::block_on(ctx,combinators(n,2)));
    }
    for(auto capacity:{64uz,1024uz}) for(auto producers:{1uz,4uz}) {
      report("mpsc_p"+std::to_string(producers)+"_w"+std::to_string(workers)+"_c"+std::to_string(capacity),
             faio::block_on(ctx,pipeline(n,producers,capacity)));
    }
    report("external_burst_"+std::to_string(workers),burst(n,workers,false));
    report("internal_burst_"+std::to_string(workers),burst(n,workers,true));
  }
  report("cross_runtime_rtt",cross_runtime(std::min(n,10000ull)));
  report("external_notification_registered",external_notification(std::min(n,10000ull)));
  report("block_on_entry",block_entry(std::min(n,10000ull)));
  }
}
