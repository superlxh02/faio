# faio 示例：从协程基础到完整应用

八个程序各自只有一个独立的 `.cpp`，包含完整场景、运行时配置和 `main`，只依赖 faio 与标准库。示例之间不包含、不链接彼此的源码，也没有公共示例头文件。

协程的六种用法全部放在 `coroutine_basics.cpp`，同步的六种原语全部放在 `coroutine_sync.cpp`；每种用法一个 `example_*`
入口函数。各函数自行定义子协程和局部数据，没有跨示例函数的复用关系。短小子协程直接用无捕获、通过参数传值的 lambda，代码中完整写出
`co_await`、`spawn` 和同步操作。

## 目录与阅读顺序

| 顺序 | 程序                                                                     | 场景与阅读重点                                           | 运行方式                                |
|----|------------------------------------------------------------------------|---------------------------------------------------|-------------------------------------|
| 1  | [coroutine_basics.cpp](coroutine_basics.cpp)                           | 商品价格查询：等待子协程、启动并发任务、后台通知、汇合和超时竞速                  | 运行后自动退出                             |
| 2  | [coroutine_sync.cpp](coroutine_sync.cpp)                               | 库存更新、配置加载、下载限流、初始化、分轮处理、订单队列                      | 运行后自动退出                             |
| 3  | [tcp_echo_server.cpp](tcp_echo_server.cpp)                             | 简单 TCP echo：接受连接，每个连接一个协程                         | `127.0.0.1:8080` 常驻服务               |
| 4  | [udp_echo_server.cpp](udp_echo_server.cpp)                             | 简单 UDP echo：按数据报回显给来源地址                           | `127.0.0.1:9090` 常驻服务               |
| 5  | [tcp_counter_server.cpp](tcp_counter_server.cpp)                       | 多客户端共享计数器：文本协议、拆包与流水线、共享状态、超时和错误处理                | `127.0.0.1:8081`；`--self-test` 自动退出 |
| 6  | [blocking_thread_pool.cpp](blocking_thread_pool.cpp)                   | 同步旧 SDK 与异步价格查询并发，阻塞期间协程心跳持续运行                    | 运行后自动退出                             |
| 7  | [tcp_echo_server_single_thread.cpp](tcp_echo_server_single_thread.cpp) | 相同 TCP echo 迁移到单线程调度，比较调度模式                       | `127.0.0.1:8080` 常驻服务               |
| 8  | [file_and_directory.cpp](file_and_directory.cpp)                       | 创建 `test/main.c`、写入 Hello World 源码、枚举目录、进入目录后读取文件 | 运行后自动退出，保留生成的目录与文件                  |

`tcp_echo_server_single_thread.cpp` 直接复制并完整实现 TCP echo 的监听与连接处理。该文件包含全部逻辑，无需查阅多线程版本或公共头文件。两个版本使用相同的端口和协议，main
中的调度模式不同。Linux 后端参数也在各程序自己的 main 中独立处理。

这八个示例也构建并验证于 Windows 的 MSVC、clang-cl 和 MinGW-w64。`windows_framework.cpp` 保留原文件名，用于独立 IOCP
引擎能力与停机排空检查。Windows 构建步骤见根目录 [README 的源码编译](../README.md#源码编译)，IOCP 后端实现见
[异步 IO](../docs/异步IO.md) 的 Windows IOCP 章节，原生网络契约见
`tests/test_windows_network.cpp`，接口说明见 [网络 IO](../docs/网络IO.md)。

[experimental/](experimental/) 子目录收录实验性特性的独立示例：`async_main.cpp` 展示注解声明的异步入口，
`async_main_execution.cpp` 与 `execution_current_thread.cpp`、`execution_multi_thread.cpp` 展示 stdexec Sender/Receiver
与 faio 协程的互操作，`execution_tcp_server.cpp` 给出完整网络服务形态。这些示例要求 Linux + GCC 16.1 + C++26，
经 `FAIO_ENABLE_EXPERIMENTAL` 与 `linux-gcc16.1-experimental-*` 预设构建，不参与上面八个示例的跨平台矩阵；
机制阐述与源码解析见 [实验性特性](../docs/实验性特性.md)。

## 构建与运行

从仓库根目录构建。macOS 使用项目的 Homebrew Clang preset：

```sh
cmake --preset macos-clang23
cmake --build --preset macos-clang23 --target coroutine_basics coroutine_sync tcp_echo_server udp_echo_server tcp_counter_server blocking_thread_pool tcp_echo_server_single_thread file_and_directory -j4
```

Linux Clang 22 双后端构建：

```sh
cmake --preset linux-clang22-dual
cmake --build --preset linux-clang22-dual --target coroutine_basics coroutine_sync tcp_echo_server udp_echo_server tcp_counter_server blocking_thread_pool tcp_echo_server_single_thread file_and_directory -j4
```

下面统一把构建目录记为 `BUILD_DIR`。macOS：

```sh
BUILD_DIR=build/macos-clang23
```

Linux：

```sh
BUILD_DIR=build/linux-clang22-dual
```

先运行无需外部客户端的程序：

```sh
"$BUILD_DIR/examples/coroutine_basics"
"$BUILD_DIR/examples/coroutine_sync"
"$BUILD_DIR/examples/blocking_thread_pool"
"$BUILD_DIR/examples/tcp_counter_server" --self-test
"$BUILD_DIR/examples/file_and_directory"
```

文件示例会在启动时的当前工作目录生成 `test/main.c`，详见第 8 节。启用 `FAIO_BUILD_TESTS` 时，这五个有限示例也会注册为
CTest，运行：

```sh
ctest --test-dir "$BUILD_DIR" -L examples --output-on-failure
```

Linux 所有八个示例都接受 `--io-backend=epoll` 或 `--io-backend=uring`，例如：

```sh
"$BUILD_DIR/examples/tcp_counter_server" --self-test --io-backend=epoll
"$BUILD_DIR/examples/tcp_echo_server_single_thread" --io-backend=uring
```

也可以用 `FAIO_TEST_IO_BACKEND=epoll|uring` 指定示例后端，显式命令行优先。不指定时采用库的默认后端选择规则；io_uring
必须在构建时启用且运行环境支持。macOS 固定使用 kqueue，不传 Linux 后端参数。

## 1. 协程基础与并发

`coroutine_basics.cpp` 的六个示例互相独立：

| 函数                              | 核心写法                                                 | 应观察到的行为                       |
|---------------------------------|------------------------------------------------------|-------------------------------|
| `example_await_and_block_on`    | 子协程 `co_await query_price(...)`，普通线程 `block_on(...)` | 等单价查询返回再计算，总价 42              |
| `example_spawn_and_join_handle` | `spawn(...)`，再 `co_await handle`                     | 两个查询已并发启动，等待结果时不占住 worker     |
| `example_spawn_detached`        | `spawn_detached(...)`                                | 主流程继续，后台发送两条订单通知；通过完成信号观察结束   |
| `example_join`                  | `co_await join(...)`                                 | 不同结果类型的任务并发完成，得到商品名和价格的 tuple |
| `example_join_all`              | `co_await join_all(vector<task<int>>)`               | 动态数量的查询同时运行，结果按供应商输入顺序排列      |
| `example_select`                | `co_await select(query, deadline)`                   | 查询先完成返回 88，定时器落选并被取消          |

`spawn_detach` 对应的实际 API 名称是 **`spawn_detached`**。

几个使用差别需要结合代码理解：

- 调用返回 `task<T>` 的协程函数只创建惰性任务。直接 `co_await` 一个子任务后，父协程等它返回再继续；不会自动创建一个独立并发任务。
- `spawn` 立即提交独立任务，并返回 `join_handle<T>`。即使后面按顺序等待两个句柄，两个任务也已经启动。句柄的返回值只能领取一次。
- `spawn_detached` 不提供结果句柄，适合无需返回值的后台通知。异常必须在任务体内处理。本例显式等完成信号，是为了保证完整观察输出。
- `join` 和 `join_all` 都在被等待时启动其分支，等待全部结束再返回，结果排列与输入顺序一致。`join` 的 `void` 分支以
  `std::monostate` 占位。
- `select` 获取先完成者，取消其余分支并等它们退出。`index` 对应原始参数下标，`value` 用 `std::get<下标>`
  访问。分支失败也算完成，获胜分支的异常会传播给调用者。取消是协作的，只有能响应取消的等待才会及时退出。
- `block_on` 用在普通线程。协程内部用 `co_await`，不能调用会阻塞线程的 `block_on`、`join_handle::get()` 或 `wait()`。

若需观察 `select` 的超时路径，可将示例中的 `query_price(88, 20ms)` 修改为 `query_price(88, 200ms)`，保留 100ms 的
deadline。预期输出“等待报价超时”，且查询分支的异步 sleep 被取消。

示例中 lambda 的使用遵循明确约束：定义于示例内部的短小协程一律使用无捕获 lambda，通过参数将值传入协程帧。不得直接调用临时的带捕获协程
lambda 并让其返回的任务异步执行，闭包可能在任务执行前已经销毁。普通同步 lambda 交给 `spawn_blocking`
时会被按值保存，不存在闭包悬空问题，但其借用的对象仍需存活至任务结束。

## 2. 协程同步

`coroutine_sync.cpp` 保留六个原语示例函数，不拆文件。每个入口自建数据与同步对象，互相独立；mutex 在同一个函数中对照手动加解锁和
guard。

| 函数                           | 原语                                                | 场景与关键点                                        |
|------------------------------|---------------------------------------------------|-----------------------------------------------|
| `example_mutex`              | `lock()` / `unlock()` 与 `scoped_lock()` / `guard` | 分别运行两组补货任务，每组三个协程各加 100，手动与 guard 写法的库存都为 300 |
| `example_condition_variable` | `condition_variable.wait(mutex, predicate)`       | 两个请求等配置加载；wait 释放锁、挂起，恢复后重获锁并检查谓词             |
| `example_semaphore`          | `semaphore.acquire_permit()`                      | 五个下载最多同时运行两个；permit 跨异步等待持有，析构归还              |
| `example_latch`              | `count_down()` / `wait()`                         | 三个初始化任务完成后开放服务；一次性倒计时，不能重置                    |
| `example_barrier`            | `arrive_and_wait()`                               | 三个分片每一轮全部完成后再进入下一轮；屏障可以重复使用                   |
| `example_mpsc`               | `sender.send()` / `receiver.recv()`               | 两个订单生产者、一个消费者；容量 2，队列满时生产者挂起，体现背压             |

mutex 的两个写法在同一个示例函数中完整展示，各自新建 mutex 和库存：

```cpp
// 手动写法：获取和释放由调用者明确完成。
co_await mutex.lock();
++inventory;
mutex.unlock();

// guard 写法：异步获取锁后，由作用域管理释放。
{
    auto guard = co_await mutex.scoped_lock(); // 返回 faio::sync::mutex::guard
    ++inventory;
} // guard 析构，自动解锁。
```

手动写法必须保证每条退出路径都调用 unlock；本例的临界区只做不会抛异常的整数递增。guard 写法在 return 或异常退出时也会解锁。
`mutex::guard` 的构造器本身不负责加锁，`scoped_lock()` 才负责先异步取锁再生成 guard，因此不要在尚未持锁时直接构造 guard。

条件变量保护的是“配置已经加载”这个状态，通知只是提醒重新检查状态。加载者必须先在锁内更新数据和谓词，再通知等待者；等待者即使晚于通知到达，也能看见状态而直接继续。

mutex 通常只保护短暂的数据访问，不要持锁等待网络请求。semaphore 则刻意在整个下载期间持有许可，用来限制并发量，这是两个不同用途。

MPSC 的 sender 可复制，receiver 只有一个。本例把两份 sender 都移动给生产者，入口不保留多余发送端；最后一个生产者退出后，消费者先排空已发送订单，再收到
`ClosedChannel`，最终处理 6 个订单。保留一份闲置 sender 会让消费者继续等待后续消息，无法识别结束。

示例中的 `join` / `join_all` 会等子任务结束，之后才销毁入口中的共享对象。日志交错顺序可能随调度变化，最终库存、并发上限和订单数应保持一致。

## 3. TCP Echo 服务

在一个终端启动：

```sh
"$BUILD_DIR/examples/tcp_echo_server"
```

在另一个终端连接：

```sh
nc 127.0.0.1 8080
```

输入一行文本后将收到相同内容。可同时建立多个终端连接；单个连接暂不发送数据不会影响其他连接的回显。服务通过 Ctrl+C 终止。

处理逻辑分为两层：监听循环 `co_await accept()`，成功后把 stream 移给 `spawn_detached(echo_connection(...))`；连接协程循环
`co_await read()`、`co_await write_all()`。等待 accept 或 read 都会让出调度线程。

TCP 是字节流，某次 `read` 可能只拿到半条文本，也可能拿到几条文本。本例按字节原样回显，无需消息解析。发送使用 `write_all`，避免把
`write` 的短写误认为整段数据已经发完。读取 0 字节表示对端关闭发送方向，连接任务结束并由 stream 析构释放资源。

## 4. UDP Echo 服务

启动服务和客户端：

```sh
"$BUILD_DIR/examples/udp_echo_server"
```

```sh
nc -u 127.0.0.1 9090
```

UDP 无 accept 语义，每次 `recv_from` 返回一条数据报及来源地址，然后 `send_to`
把同样内容回给该地址。它保留消息边界，因此一个数据报对应一次回显；没有等待多条数据凑成消息的逻辑。

缓冲区为 65507 字节，覆盖本例 IPv4 UDP 的最大有效载荷。0 字节代表合法的空数据报，仍会回显；不能像 TCP 那样将其解释为 EOF。UDP
不保证送达、顺序与去重，本例未实现重传协议。

## 5. TCP 应用示例：共享计数器服务

该示例模拟多个客户端对同一组统计项执行读取与更新。例如各个入口统计 `visits`，管理客户端设置、查询和删除 `stock`
。数据保存在进程内存中，服务重启后消失。

启动后用 `nc 127.0.0.1 8081` 交互：

```sh
"$BUILD_DIR/examples/tcp_counter_server"
```

协议是区分大小写的文本命令，一行一条请求、一行一条响应，支持 LF 和 CRLF。键不能含空白，最多 32 字节；值是有符号 64 位十进制整数。

| 请求             | 响应示例       | 含义                      |
|----------------|------------|-------------------------|
| `SET stock 12` | `OK`       | 设置一个整数值                 |
| `GET stock`    | `VALUE 12` | 读取现有值；不存在返回 `NOT_FOUND` |
| `INCR stock`   | `VALUE 13` | 原子递增并返回新值；不存在从 0 开始加 1  |
| `DEL stock`    | `DELETED`  | 删除键；不存在返回 `NOT_FOUND`   |
| `QUIT`         | `BYE`      | 回复后关闭当前连接               |

一次交互可以输入：

```text
SET stock 12
GET stock
INCR stock
DEL stock
GET stock
QUIT
```

对应响应为 `OK`、`VALUE 12`、`VALUE 13`、`DELETED`、`NOT_FOUND`、`BYE`。命令不会关闭连接，只有 `QUIT` 或连接错误/终止错误才结束该连接。

从服务端的三个函数阅读：`accept_clients` 启动每个连接协程，`counter_session` 负责消息分帧、期限和收发，`execute_command`
解析命令并访问共享数据。

它解决了 echo 之外的实际应用问题：

1. **消息边界**：`BufReader` 保留预读字节，`read_line` 组合拆开的请求并分离流水线请求。不能把一次 TCP read 当成一条业务命令，也不能绕过
   BufReader 直接读底层 stream。
2. **共享状态**：所有连接共享一个 `counter_store`，使用协程 mutex。INCR 的读、加、写必须处在同一个临界区，防止并发更新丢失。解析在加锁前完成，网络发送在解锁后执行。
3. **资源边界**：单条请求最多 1024 字节（含换行），最多保存 256 个键；超长请求响应 `ERR too_long` 后关闭，避免继续把残余数据误解析成新命令。容量不足响应
   `ERR capacity`，连接仍可处理其他请求。
4. **超时**：每条完整请求限时 30 秒，超时响应 `ERR timeout` 后关闭；不是每来一个字节重置计时。每个响应的完整发送限时 5
   秒，超时直接关闭。这里用 select 将 I/O 任务和 deadline 竞速，并在返回前收束落选分支。
5. **协议错误**：无效命令、参数、键、整数和递增溢出分别返回 `ERR command`、`ERR arguments`、`ERR key`、`ERR integer`、
   `ERR overflow`；这些错误不会中断其他客户端，也不立即关闭当前连接。
6. **连接结束**：正常 EOF 直接退出；半关闭时还剩无换行的命令，回复 `ERR incomplete` 并关闭，不执行残缺命令。detached
   任务在内部处理异常，stream 的所有权随连接协程结束而释放。

有限演示：

```sh
"$BUILD_DIR/examples/tcp_counter_server" --self-test
```

程序绑定临时端口，两个并发客户端各对 `visits` 递增五次，第三个客户端确认总量是
10，并验证库存设置、查询、递增、删除、错误整数和未知命令。第三个客户端把第一条请求拆成两次发送，并把后续请求流水线发送；服务需要正确处理所有响应。有限模式接受三个连接、等待全部连接结束后自动退出，不依赖外部服务。

长期服务使用 detached 连接任务；有限演示保留并等待连接句柄，便于确认全部结束。共享 store 放在 main 的 block_on
外部，覆盖所有派生连接任务的生命周期。

## 6. 阻塞线程池与协程调度的协作

`blocking_thread_pool.cpp` 的商品详情需要同时获得库存和价格：

- 库存来自没有异步 API 的旧 SDK：普通同步函数 `legacy_inventory_query`，通过 `spawn_blocking` 放到独立线程池；
  `sleep_for(400ms)` 模拟它的阻塞等待。
- 价格来自异步服务：`async_price_query` 使用异步 sleep 模拟网络等待，通过普通 `spawn` 走协程调度。
- 心跳同样是协程：每 50ms 输出一次状态，展示阻塞等待期间调度线程仍在运行其他任务。

运行：

```sh
"$BUILD_DIR/examples/blocking_thread_pool"
```

程序只配置 **一个协程 worker**，阻塞池上限为两个线程。预期行为：库存 SDK 启动后，价格查询约 80ms 完成，心跳持续输出，库存约
400ms 后完成，最终汇合成“库存 12，单价 299”。具体顺序和时间会受系统负载影响。日志包含线程 ID，可观察阻塞 SDK 与价格/心跳使用不同线程。

主流程等待库存句柄时使用 `co_await inventory`，因此唯一协程 worker 可以继续执行价格查询和心跳。这里若改用
`inventory.get()`，会阻塞 worker；若直接在协程中调用旧 SDK，也会阻塞 worker。异步网络操作和定时器已有可等待接口，直接使用协程，不要再转交给阻塞线程池。

本例以定时器和 sleep_for 模拟外部服务，使程序能够独立运行；替换为实际 SDK 与网络请求时，任务分工保持不变。

## 7. 单线程 TCP 服务

先结束多线程 TCP 服务，再启动：

```sh
"$BUILD_DIR/examples/tcp_echo_server_single_thread"
```

客户端仍然是 `nc 127.0.0.1 8080`。本文件独立定义完整监听和连接处理，功能与 `tcp_echo_server.cpp` 相同；配置区别直接写在
main 中：

```cpp
faio::runtime::configure(faio::config_builder{}
    .set_mode(faio::runtime::mode::current_thread)
    .build());
faio::block_on(example_tcp_echo_server(8080));
```

监听任务和全部连接协程由调用 block_on 的当前线程驱动。一个连接在等待 I/O
时，该线程切换去处理其他就绪协程，仍然支持多连接。可同时打开两个客户端并仅向其中一个发送消息，可观察到其回显不受另一空闲连接影响。

`set_num_workers(1)` 只是多线程模式下的一个后台 worker，不能替代 `set_mode(current_thread)`。单线程模式不创建后台协程
worker；未调用 block_on 时，已提交协程不会自行推进。独立的阻塞池和其他辅助服务仍可能使用线程，单线程调度描述的是协程执行方式，不是整个进程只能有一个操作系统线程。

四个长期服务示例均用 Ctrl+C 结束进程，本例未实现应用级优雅停机协议。运行时模式必须在首次使用前配置；修改模式需要重新启动示例程序。

## 8. 文件与目录操作

`file_and_directory.cpp` 用一个协程顺序完成文件操作场景。每一步都在这个独立文件中直接写出，没有公共示例工具或额外的封装类，并附有中文注释。

从仓库根目录运行：

```sh
cmake --build --preset macos-clang23 --target file_and_directory -j4
./build/macos-clang23/examples/file_and_directory
```

运行目录指 **启动程序时的当前工作目录**。上述命令会在仓库根目录生成 `test/main.c`；如果先切换到其他目录，再用可执行文件的绝对路径启动，文件就生成在那个目录中。

程序按以下顺序展示：

1. 保存运行目录，用 `co_await faio::fs::create_dir_all(...)` 创建 `test`。
2. 用 `File::create` 创建 `test/main.c`，`write_all` 完整写入 C 源码，然后等待 `close` 完成。
3. 用 `read_dir` 打开运行目录，循环等待 `next_entry`，在输出中展示新建的 `test/` 目录。
4. 用 `std::filesystem::current_path(test_directory)` 真正进入 `test`，打印进入后的工作目录。
5. 用相对路径 `read_dir(".")` 枚举 `test` 中的所有条目，包括已有文件、子目录和隐藏文件，跳过 `.` 与 `..`。
6. 用相对路径 `read_to_string("main.c", 4096)` 读取源码，检查与写入内容一致后完整打印，再恢复原工作目录。

写入和读回的 `main.c` 内容为：

```c
#include <stdio.h>

int main(void) {
    printf("Hello, world!\n");
    return 0;
}
```

`next_entry` 返回 `expected<optional<DirEntry>>`：外层错误代表枚举失败，内层空值代表正常结束。示例分别判断这两种情况，并使用
`file_name` 和异步 `file_type` 展示条目的名称与类型。目录枚举顺序由文件系统决定。

文件与目录操作都通过 faio 的可等待接口执行。faio 没有切换工作目录的接口，这一步使用标准库；工作目录属于整个进程，因此示例顺序执行并先完成前面的操作，避免其他协程同时使用相对路径。进入
`test` 后无论正常返回还是发生异常，都安排恢复原目录。

重复运行时，已存在的 `test` 目录可以继续使用，`main.c` 会被截断后重新写入，其他条目会保留。程序结束后保留 `test/main.c`
，便于查验。CTest 使用构建目录下的 `examples/file_and_directory_workdir` 作为工作目录，所以测试生成的文件位于其中的
`test/main.c`。
