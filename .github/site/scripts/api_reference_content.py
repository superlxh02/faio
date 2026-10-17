"""Public API descriptions; declarations and overloads come from Doxygen XML."""

GROUPS = [
 ('tasks','协程与并发','任务创建、启动、汇合、竞速和结构化作用域。'),
 ('runtime','运行时与配置','运行时模式、容量、生命周期与当前协程查询。'),
 ('network','网络 I/O','TCP、UDP、地址、解析和套接字配置。'),
 ('filesystem','文件与目录','文件读写、路径操作、元数据和目录枚举。'),
 ('time','时间与超时','单调时钟等待、周期定时器和 I/O deadline。'),
 ('sync','协程同步','互斥锁、信号量、条件变量、闩、屏障和 MPSC 通道。'),
 ('io','缓冲与流算法','缓冲区、完整传输、读写适配和原生描述符。'),
 ('errors','错误与日志','expected、Error、错误域与日志入口。'),
]


def export(public, group, description, source=None, **kwargs):
    header = {'network':'faio/net.hpp','filesystem':'faio/fs.hpp','io':'faio/io.hpp','errors':'faio/faio.hpp'}.get(group,'faio/faio.hpp')
    return dict(public=public, source=source or public, group=group, header=header, description=description, **kwargs)


EXPORTS = [
 export('faio::task','tasks','惰性的、移动独占的协程任务。',members=['task','with_priority'],fields={'value_type':'协程完成后的值类型；void 表示无业务返回值。'},note='调用协程函数仅创建任务；co_await、spawn 或 block_on 才驱动执行。移动后原任务为空。'),
 export('faio::join_handle','tasks','并发任务的一次性结果句柄。',members=['join_handle','done','request_stop','stop_token','wait','get'],note='协程中 co_await 句柄；普通线程使用 wait/get。结果只能领取一次，丢弃句柄不会自动停止任务。'),
 export('faio::scope_context','tasks','scope 回调中用于派生和停止子任务的作用域。',members=['spawn','request_stop','stop_token']),
 export('faio::config_builder','runtime','在运行时启动前构造和验证运行配置。',note='先配置再调用 block_on/spawn。配置运行时后再次修改 builder 不影响已经启动的服务。'),
 export('faio::runtime::config','runtime','由 config_builder::build() 返回的配置值。',source='faio::runtime::detail::runtime_config',members=[],note='优先通过 config_builder 设置配置；配置对象可以按值传入 runtime::configure。'),
 export('faio::net::TcpStream','network','已经连接的全双工 TCP 字节流。',source='faio::net::detail::TcpStream',exclude=['TcpStream','resource'],bindings={'Stream':'TcpStream','Addr':'SocketAddr'},note='同一连接可同时进行一个读和一个写；同方向冲突返回 EBUSY。非空读取返回 0 才表示 EOF。',example='auto stream = co_await faio::net::TcpStream::connect(*address);\nif (!stream) co_return;\nchar buffer[4096];\nauto count = co_await stream->read(buffer);'),
 export('faio::net::TcpListener','network','TCP 监听器和异步连接接纳接口。',source='faio::net::detail::TcpListener',exclude=['TcpListener','resource'],bindings={'Listener':'TcpListener','Stream':'TcpStream','Addr':'SocketAddr'},example='auto listener = faio::net::TcpListener::bind(*address);\nif (!listener) co_return;\nauto accepted = co_await listener->accept();'),
 export('faio::net::TcpSocket','network','连接前可配置选项的 TCP 套接字。',source='faio::net::detail::TcpSocket',exclude=['TcpSocket','resource'],bindings={'T':'TcpSocket'}),
 export('faio::net::UdpSocket','network','连接态和非连接态 UDP 数据报套接字。',source='faio::net::detail::UdpSocket',exclude=['UdpSocket','resource'],bindings={'Datagram':'UdpSocket','Addr':'SocketAddr'},note='UDP 零长度数据报是有效消息，不能按 TCP EOF 处理。'),
 export('faio::net::SocketAddr','network','包含 IP 地址、端口和 IPv6 scope id 的值类型。',source='faio::net::detail::SocketAddr'),
 export('faio::net::Ipv4Addr','network','IPv4 地址值。',source='faio::net::detail::Ipv4Addr'),
 export('faio::net::Ipv6Addr','network','IPv6 地址值。',source='faio::net::detail::Ipv6Addr'),
 export('faio::net::HostPort','network','用于主机名解析的 host/service 参数值。',source='faio::net::detail::HostPort',fields={'host':'主机名或数字 IP；按值保存。','service':'服务名或端口的字符串形式。'}),
 export('faio::fs::File','filesystem','异步文件句柄，支持游标读写、显式偏移和持久化。',exclude=['resource'],note='借用缓冲区必须保留到操作完成；同一逻辑游标上的操作应协调顺序。',example='auto opened = co_await faio::fs::File::open("data.txt");\nif (!opened) co_return;\nchar buffer[1024];\nauto count = co_await opened->read(buffer);'),
 export('faio::fs::OpenOptions','filesystem','可组合的文件打开、创建、追加和截断选项。',exclude=['flags','append_enabled','permissions']),
 export('faio::fs::ReadDir','filesystem','以流式方式逐条枚举目录。'),
 export('faio::fs::DirEntry','filesystem','一个目录条目的名称、路径和元数据查询。',exclude=['DirEntry']),
 export('faio::fs::Metadata','filesystem','文件类型、大小、权限和时间信息。',exclude=['Metadata']),
 export('faio::fs::Permissions','filesystem','文件系统访问权限的值类型。'),
 export('faio::fs::DirBuilder','filesystem','带递归和权限选项的目录创建器。'),
 export('faio::fs::seek_from','filesystem','文件定位的起点和偏移量。',fields={'origin':'定位基准。','base':'定位起点。','offset':'相对基准的有符号字节位移。'},enum_values={'origin':{'start':'从文件开头计算。','current':'从当前逻辑游标计算。','end':'从文件末尾计算。'}}),
 export('faio::fs::recursive_remove_options','filesystem','递归删除的边界与符号链接处理选项。',fields={'max_depth':'同时打开的目录数上限，包含根目录；默认 256。','max_state_bytes':'根路径与持有名称的内存预算；默认 1 MiB。','batch_size':'每个文件 job 的遍历步数；默认 64，上限 4096。'}),
 export('faio::sync::mutex','sync','等待时挂起协程的互斥锁。',exclude=['lock_uncancellable'],note='优先使用 scoped_lock() 的 RAII guard，避免异常路径忘记解锁。'),
 export('faio::sync::semaphore','sync','限制同时持有资源数量的协程信号量。',exclude=['acquire_uncancellable']),
 export('faio::sync::condition_variable','sync','基于共享谓词通知和唤醒协程。',note='等待前已持有互斥锁；等待期间释放锁，恢复后重新持有锁。始终在锁内检查谓词。'),
 export('faio::sync::latch','sync','一次性的异步倒计数完成信号。'),
 export('faio::sync::barrier','sync','可重复使用的分轮协程屏障。'),
 export('faio::sync::mpsc','sync','固定容量的多生产者、单消费者通道。',members=['make'],note='发送端可复制，接收端移动独占；同一时刻只能有一个未完成的 recv。'),
 export('faio::sync::mpsc::sender','sync','MPSC 的可复制发送端。',parent='faio::sync::mpsc'),
 export('faio::sync::mpsc::receiver','sync','MPSC 的移动独占接收端。',parent='faio::sync::mpsc'),
 export('faio::io::io_buffer','io','移动独占的 I/O 字节缓冲区。',note='size 是已初始化范围，capacity 是可写容量。移动给操作后应等待完成再使用返回的缓冲区。'),
 export('faio::io::io_transfer','io','拥有型 I/O 操作返回的缓冲区与传输量。',fields={'buffer':'操作完成后归还的拥有型缓冲区。','bytes':'本次实际传输的字节数。'}),
 export('faio::io::read_buf','io','区分已初始化范围与剩余可写空间的借用读缓冲视图。'),
 export('faio::io::shared_const_buffer','io','可以供多个发送操作共享的不可变缓冲区。'),
 export('faio::io::BufReader','io','有界预读适配器。',note='底层 reader 必须覆盖适配器和全部等待；不得绕过未消费的预读内容直接读取底层对象。'),
 export('faio::io::BufWriter','io','有界缓冲写适配器。',note='析构不执行异步 flush；退出前显式等待 flush 或 shutdown。'),
 export('faio::io::BufStream','io','为可读写流组合读缓冲与写缓冲。'),
 export('faio::io::io_context','io','选择 I/O 操作所属上下文的公开引用。',members=['io_context','valid','operator bool','stopped','stop_token','current','balanced_context']),
 export('faio::io::unix::OwnedFd','io','Unix 原生文件描述符的 RAII 所有者。',note='仅在 Unix 平台提供。复制所有权必须使用显式复制操作。'),
 export('faio::io::unix::AsyncFd','io','Unix 非阻塞原生描述符的就绪适配器。',exclude=['resource','domain','acquire','native'],note='仅在 Unix 提供；普通磁盘文件使用 fs::File。就绪通知不能替代实际非阻塞系统调用。'),
 export('faio::io::unix::AsyncFdReadyGuard','io','一次就绪观察结果及非阻塞尝试。',members=['ready','clear_ready','try_io']),
 export('faio::Error','errors','携带错误来源、原生错误码和已完成传输进度的错误值。',fields={'ErrorCode':'faio 自定义错误码；具体数值与系统错误域分开解释。'},enum_values={'ErrorCode':{'EmptySqe':'没有可用的提交队列项。','InvalidAddresses':'没有可以使用的目标地址。','ClosedChannel':'通道已关闭。','UnexpectedEOF':'缓冲区填满前遇到 EOF。','WriteZero':'非空写入没有产生任何进度。','TooLongTime':'请求的等待时间超出支持范围。','PassedTime':'指定时间已经过去。','InvalidSocketType':'原生 socket 类型不匹配。','ReuniteFailed':'尝试合并不同来源的读写半部。'}}),
]

EXPORTS += [
 export('faio::net::accept_options','network','控制新接纳连接的首次 I/O 上下文归属。',members=[],fields={'placement':'归属策略；默认 balanced，在同一运行时的活跃上下文间轮转。','target':'仅 explicit_context 策略使用；必须非空且未停止。'}),
 export('faio::net::DatagramMessage','network','数据报接收结果与截断标志。',source='faio::net::detail::DatagramMessage',concrete=True,bindings={'Addr':'SocketAddr'},fields={'copied_bytes':'实际复制到负载缓冲区的字节数。','peer':'发送方地址。','truncated':'负载因缓冲空间不足而被截断。','control_truncated':'辅助控制数据被截断。','control_bytes':'实际收到的控制信息字节数。','flags':'本次接收的原生消息标志。'}),
 export('faio::net::DatagramSend','network','批量发送中一个借用数据报的描述。',source='faio::net::detail::DatagramSend',concrete=True,bindings={'Addr':'SocketAddr'},fields={'bytes':'发送的只读负载，须保留到操作完成。','peer':'可选目的地址；连接态 socket 可省略。'}),
 export('faio::net::OwnedDatagram','network','拥有缓冲区的数据报接收结果。',source='faio::net::detail::OwnedDatagram',concrete=True,bindings={'Addr':'SocketAddr'},fields={'buffer':'完成后归还的拥有型缓冲区。','message':'负载量、来源地址及截断信息。'}),
 export('faio::net::owned_native_socket','network','平台原生 socket 的移动独占 RAII 所有者。',source='faio::net::detail::owned_native_socket',exclude=['validate_import']),
 export('faio::io::borrowed_buffer','io','显式声明借用可写字节范围的 I/O 参数。',fields={'bytes':'在操作完成前保持内存与地址有效的字节视图。'}),
 export('faio::io::Ready','io','资源就绪观察结果。',fields={},note='就绪只是提示；仍需通过非阻塞操作判断实际能否读取或写入。'),
]

ENUMS = [
 ('faio','task_priority','tasks','任务优先级元数据。',{'low':'低优先级。','normal':'默认优先级。','high':'高优先级。'}),
 ('faio','error_domain','errors','原生错误码的解释来源。',{'faio':'库定义错误。','posix':'POSIX errno。','resolver':'名称解析错误。','win32':'Win32 错误码。','winsock':'Winsock 错误码。'}),
 ('faio::runtime','mode','runtime','运行时的调度模式。',{'current_thread':'在当前线程驱动任务。','multi_thread':'在多个 worker 上调度任务。'}),
 ('faio::runtime','io_backend','runtime','Linux 编译时选择的 I/O 后端。',{'IO_EPOLL':'epoll 后端。','IO_URING':'io_uring 后端。'}),
 ('faio::io','Interest','io','等待就绪的方向集合。',{'none':'不选择方向。','readable':'读就绪。','writable':'写就绪。','read_write':'同时观察读写。'}),
 ('faio::io','ShutdownBehavior','network','socket 半关闭方向。',{'Read':'停止接收。','Write':'停止发送。','Both':'停止双向传输。'}),
 ('faio::io','shutdown_policy','runtime','运行时的退出策略。',{'drain':'等待已接受任务排空；任务需有退出路径。','cancel_all':'先请求任务停止，再等待排空。'}),
 ('faio::time','MissedTickBehavior','time','错过周期触发后的调度行为。',{'Burst':'尽快补发错过的触发。','Delay':'从当前时间重新等待一个周期。','Skip':'跳过错过的触发并对齐到后续周期点。'}),
 ('faio::net','accept_placement','network','新接纳连接的 I/O 归属策略。',{'listener_local':'与监听器使用相同上下文。','balanced':'在同一运行时的活跃上下文中轮转。','explicit_context':'使用 accept_options::target。'}),
]

FUNCTION_GROUPS = [
 dict(source='faio',public='faio',group='tasks',members=['block_on','spawn','spawn_detached','spawn_blocking','join','join_all','select','scope']),
 dict(source='faio::runtime',public='faio::runtime',group='runtime',members=['configure','shutdown']),
 dict(source='faio::this_coro',public='faio::this_coro',group='runtime',members=['stop_token','scheduler','worker_id','priority','yield','yield_if_needed']),
 dict(source='faio::net::detail',public='faio::net',group='network',members=['lookup_host']),
 dict(source='faio::fs',public='faio::fs',group='filesystem'),
 dict(source='faio::time',public='faio::time',group='time'),
 dict(source='faio::io',public='faio::io',group='io',members=['take','chain','split','read_exact','write_all','write_all_buf','read_to_end','read_to_string','copy','copy_bidirectional','read_until','read_line','lines','split_by_delimiter','read_integer','write_integer','empty','repeat','sink','duplex','current_context']),
 dict(source='faio::log',public='faio::log',group='errors',members=['logger']),
]

# Each entry is brief, completion/result semantics, and optional constraints.
METHODS = {
 'block_on':('在普通线程驱动并等待一个异步任务组。','返回任务的 T 结果；T 为 void 时无返回值，任务异常在调用线程重抛。','禁止在运行时 worker 调用；协程内部使用 co_await。'),
 'spawn':('立即提交一个独立并发任务。','返回 join_handle<T>，通过 co_await 或普通线程 get() 领取一次结果。','任务移动到运行时；提交时不会等待任务完成。'),
 'spawn_detached':('提交无需结果句柄的后台任务。','无返回值；任务继续执行到完成。','任务内部必须处理异常；借用数据必须覆盖后台任务生命周期。'),
 'spawn_blocking':('把同步函数提交到独立的有界阻塞线程池。','返回可等待的结果句柄；等待后取得函数结果或传播异常。','等待不会阻塞协程 worker；任意同步函数不能被强制中断。'),
 'join':('并发运行固定数量的任务并等待全部完成。','等待后得到按输入顺序排列的 tuple；void 分支对应 std::monostate。','任一分支失败时在传播异常前收束已经启动的分支。'),
 'join_all':('并发运行动态数量的同类型任务。','等待后得到按输入顺序排列的结果集合。','任务集合的所有权移入组合操作；void 任务只等待完成。'),
 'select':('并发竞速多个任务，选择先完成的分支。','等待后得到含 index 和 variant value 的结果；index 是原始参数下标。','请求未获胜分支取消并等待退出，之后才返回。'),
 'scope':('建立退出前排空子任务的结构化作用域。','返回惰性任务；等待后取得 body 的结果或传播作用域错误。','通过 scope_context::spawn 派生子任务；失败会请求同组停止。'),
 'with_priority':('为尚未提交的任务设置优先级元数据。','返回移动后的任务，可继续传给 spawn 或 block_on。','当前队列仍按 FIFO 调度，不承诺严格的抢占优先级。'),
 'done':('立即查询任务是否已经完成。','完成时返回 true；空句柄返回 false，不消费结果。'),
 'request_stop':('请求相关任务协作停止。','无返回值；请求停止不表示任务已经退出。','仍需等待结果或作用域收束，才能释放任务借用的资源。'),
 'stop_token':('获取当前对象的协作停止令牌。','返回 std::stop_token；this_coro 查询需 co_await 才取得当前任务令牌。'),
 'wait':('等待异步条件或任务完成。','等待后无业务结果；任务句柄 wait() 不领取结果。'),
 'get':('在普通线程等待并领取一次任务结果。','返回 T；任务抛出的异常在这里重抛。','禁止在 worker 调用。结果只能领取一次。'),
 'build':('取得构建器当前配置的值副本。','返回 runtime::config；之后修改 builder 不会修改该副本。'),
 'configure':('在首次使用前设置默认运行时。','无返回值；成功后首次 block_on/spawn 使用此配置。','已启动或已经关闭时抛 logic_error；非法配置会被拒绝。'),
 'scheduler':('查询当前任务所属调度器。','co_await 后取得借用的调度器引用，不拥有运行时。'),
 'worker_id':('查询当前恢复所在 worker 的编号。','co_await 后取得本次恢复的 worker id。','协程可能迁移，不能将此值当作永久线程身份。'),
 'priority':('查询当前任务的优先级元数据。','co_await 后取得 task_priority 值。'),
 'yield':('主动让出执行权并重新入队。','co_await 后在下一次调度继续，无业务返回值。'),
 'yield_if_needed':('依据协作预算决定是否让出执行权。','co_await 后继续执行，无业务返回值。'),
 'read':('读取字节到调用方提供的缓冲区。','等待后为 expected<size_t>；拥有型重载为 expected<io_transfer>。','允许短读。非空流式读取成功返回 0 表示 EOF；缓冲区必须保留到完成。'),
 'write':('将提供的字节写入目标。','等待后为 expected<size_t>；拥有型重载为 expected<io_transfer>。','允许短写；需要完整写入时使用 write_all。'),
 'read_exact':('反复读取，直到填满整个缓冲区。','返回 expected；成员接口成功无业务值，io::read_exact 成功返回填充字节数。','提前 EOF 返回 UnexpectedEOF；error().transferred() 保留已完成进度。'),
 'read_bytes':('读取指定缓冲区的全部字节。','等待后返回 expected<void>，失败包含已经读取的进度。'),
 'write_all':('反复处理短写，直到所有输入字节写出。','等待后返回 expected<void>。','非空写入零进度时失败；错误不回滚已经写出的字节。'),
 'peek':('查看可读字节而不消费流或数据报。','等待后返回本次可见字节数的 expected 结果。'),
 'read_vectored':('把一次读取分散到多个缓冲片段。','等待后返回 expected<size_t>，表示实际读取的总字节数。','描述符和负载均需保持有效；允许短读。'),
 'write_vectored':('把多个缓冲片段聚集写出。','等待后返回 expected<size_t>，表示实际写出的总字节数。','不保证写完全部片段；调用方按返回进度继续。'),
 'read_v':('使用多个缓冲区发起分散读取。','与 read_vectored 对应重载相同。'),
 'write_v':('使用多个缓冲区发起聚集写入。','与 write_vectored 对应重载相同。'),
 'read_owned':('分配拥有型 vector 后执行一次读取。','等待后返回 expected<vector<char>>；成功 vector 仅含实际读到的字节。'),
 'write_owned':('移动接管 vector 并执行一次写入。','等待后返回 expected<size_t>，表示实际写出量。','允许短写；vector 在操作完成前由任务持有。'),
 'write_all_buf':('写出借用缓冲区中的全部字节。','等待后返回 expected<void>。','缓冲区在操作完成前必须有效；失败保留部分传输进度。'),
 'read_buf':('读取到读缓冲区未初始化的尾部并推进有效范围。','等待后返回本次读取量的 expected 结果。'),
 'flush':('提交缓冲中尚未写出的字节。','等待后返回 expected<void>。','BufWriter 失败时保留未提交部分；无用户缓冲的流不表示磁盘持久化。'),
 'close':('关闭句柄或通道端点。','异步句柄需等待并检查 expected 结果；通道端点 close() 为同步无返回值。','关闭异步资源前应协调在途操作；关闭请求不等于已经完成关闭。'),
 'shutdown':('结束指定方向的 I/O，或按策略关闭运行时。','I/O 对象等待后得到 expected<void>；runtime::shutdown 为同步无返回值。'),
 'fd':('借用底层原生描述符。','返回原生描述符值，不转移所有权。','不能在异步操作期间绕过对象的并发与生命周期约定。'),
 'as_native_handle':('借用平台原生句柄。','返回原生句柄值；所有权仍属于当前对象。'),
 'native_handle':('借用平台原生句柄。','返回句柄值；调用方不负责关闭。'),
 'context':('获取对象所属 I/O 上下文。','返回 io_context 引用值，不改变资源的所属上下文。'),
 'ready':('观察指定方向的就绪状态。','等待后返回 expected<io::Ready>；ReadyGuard::ready() 直接读取已观察结果。','就绪可以是假就绪，仍需要实际非阻塞操作确认。'),
 'readable':('等待资源的读方向就绪。','等待后返回 expected<io::Ready>，不消费字节。'),
 'writable':('等待资源的写方向就绪。','等待后返回 expected<io::Ready>，不写入字节。'),
 'try_io':('执行一次短的非阻塞原生操作。','返回回调的 expected 结果；暂不可用时由调用方处理。','回调不得阻塞或递归操作同一方向。'),
 'async_io':('执行非阻塞回调，暂不可用时等待就绪后重试。','等待后返回回调的 expected 结果。','回调与参数跨挂起保存；回调本身必须是短非阻塞操作。'),
 'into_native':('导出原生资源并转移所有权。','成功返回 expected<owned_native_socket> 或相应的原生拥有者。','在途操作未完成时可能失败，导出后原包装不再拥有资源。'),
 'from_native':('接管已拥有的原生资源。','成功返回对应资源包装的 expected 结果。','会验证资源类型与上下文；失败遵循 RAII 所有权约定。'),
 'connect':('连接目标地址或主机名。','TCP 等待后返回 expected<TcpStream>；UDP 等待后返回 expected<void>。','主机名重载先解析地址；数字地址重载直接发起连接。'),
 'bind':('把套接字绑定到地址并按类型建立监听或数据报资源。','数字地址工厂同步返回 expected 对象；主机名重载等待后返回 expected 对象。'),
 'listen':('把已绑定的 TCP socket 转为监听器。','成功返回 expected<TcpListener>，转移 socket 所有权。'),
 'accept':('等待并接纳一个连接。','等待后为 expected<pair<TcpStream,SocketAddr>>；Unix 重载使用相应流与地址。'),
 'accept_many':('在有界批次内接纳多个连接。','等待后返回连接结果集合的 expected 值。','批量上限限制一次处理量，不表示全局连接上限。'),
 'new_v4':('创建尚未连接的 IPv4 套接字。','返回 expected<TcpSocket> 或对应类型，失败含系统错误。'),
 'new_v6':('创建尚未连接的 IPv6 套接字。','返回 expected<TcpSocket> 或对应类型。'),
 'unbound':('创建尚未绑定的 UDP 套接字。','返回 expected<UdpSocket>。'),
 'split':('把字节流分成借用式读半部与写半部。','返回 pair<ReadHalf,WriteHalf>；io::split 返回对应适配器。','底层流必须覆盖借用期；读写方向分别协调。'),
 'into_split':('消费字节流并取得拥有式读写半部。','返回 pair<OwnedReadHalf,OwnedWriteHalf>。','半部可分别移动到不同协程。'),
 'share':('为数据报资源创建共享包装。','返回共享同一资源的套接字包装。','共享所有权不放宽相同方向的并发操作约束。'),
 'local_addr':('查询本地绑定地址。','返回 expected 地址值。'),
 'peer_addr':('查询连接对端地址。','返回 expected 地址值；未连接时返回系统错误。'),
 'send':('向已连接的 UDP 对端发送一个数据报。','等待后返回 expected 字节数或拥有型 io_transfer。'),
 'send_to':('向指定目标发送一个数据报。','等待后返回 expected 字节数或拥有型 io_transfer。'),
 'send_many':('批量发送一组数据报。','等待后返回已处理消息数量的 expected 结果。'),
 'recv':('接收已连接 UDP 对端的数据报。','等待后返回 expected 字节数或拥有型传输结果。','零长度数据报合法，不表示 EOF。'),
 'recv_from':('接收一个数据报及其来源地址。','等待后返回包含字节数和来源地址的 expected 结果。'),
 'peek_from':('查看数据报与来源地址而不消费它。','等待后返回包含可见字节数和来源地址的 expected 结果。'),
 'recv_many':('批量接收数据报。','等待后返回本批次消息结果集合的 expected 值。'),
 'send_message':('发送含向量负载和辅助控制信息的数据报。','等待后返回 expected<size_t>。','辅助信息格式与可用标志受平台限制。'),
 'recv_message':('接收负载和辅助控制信息。','等待后返回包含传输量、来源地址与控制信息的 expected 值。'),
 'recv_buf':('把数据报接收到读缓冲的可写尾部。','等待后返回字节数的 expected 结果。'),
 'recv_buf_from':('接收数据报到读缓冲，同时保留来源地址。','等待后返回包含字节数与来源地址的 expected 结果。'),
 'write_zc':('请求零拷贝或等价发送。','等待后返回实际传输量的 expected 结果。','是否使用零拷贝受后端与平台能力限制。'),
 'lookup_host':('异步解析主机名和服务，得到可尝试的地址列表。','等待后返回 expected<vector<SocketAddr>>，保持解析结果顺序。','数字地址可走快路径；名称解析错误具有独立 resolver 错误域。'),
 'open':('按选项打开文件。','等待后返回 expected<File>。','未指定选项的便捷重载以只读模式打开已有文件。'),
 'create':('创建或截断一个可写文件。','等待后返回 expected<File>。','已有文件内容会被截断；需要拒绝覆盖时使用 OpenOptions::create_new。'),
 'read_at':('在显式文件偏移读取，不推进逻辑游标。','等待后返回 expected<size_t> 或拥有型 io_transfer。'),
 'write_at':('在显式文件偏移写入，不推进逻辑游标。','等待后返回 expected<size_t> 或拥有型 io_transfer。'),
 'read_vectored_at':('在显式文件偏移执行分散读取。','等待后返回 expected<size_t>。'),
 'write_vectored_at':('在显式文件偏移执行聚集写入。','等待后返回 expected<size_t>。'),
 'seek':('调整文件逻辑游标。','等待后返回 expected<uint64_t>，表示新的绝对偏移。'),
 'metadata':('读取文件或路径的元数据。','异步查询返回 expected<Metadata>；已缓存值的访问遵循签名。'),
 'permissions':('读取当前权限值。','返回 Permissions，文件异步查询重载返回 expected<Permissions>。'),
 'set_len':('截断或扩展文件到指定长度。','等待后返回 expected<void>。','扩展的空洞和持久化行为由文件系统决定。'),
 'set_permissions':('设置文件或路径权限。','等待后返回 expected<void>。'),
 'sync_data':('请求同步文件数据。','等待后返回 expected<void>。','元数据持久化范围取决于平台；需要完整同步时使用 sync_all。'),
 'sync_all':('请求同步文件数据及相关元数据。','等待后返回 expected<void>。','成功语义仍受操作系统和文件系统保证范围限制。'),
 'try_clone':('显式复制文件句柄。','等待后返回 expected<File>。','复制句柄后共享底层文件状态的约定以平台和接口规则为准。'),
 'next_entry':('异步读取下一个目录条目。','返回 expected<optional<DirEntry>>：空 optional 表示枚举结束，外层错误表示失败。'),
 'path':('取得目录条目的路径。','返回路径值或常量引用，遵循当前重载签名。'),
 'file_name':('取得不包含父目录的条目名称。','返回文件名路径值。'),
 'file_type':('查询文件系统类型。','返回 file_type；异步条目查询返回 expected<file_type>。'),
 'len':('查询元数据记录的文件长度。','返回字节数。'),
 'is_file':('判断元数据是否为普通文件。','返回 bool。'),
 'is_dir':('判断元数据是否为目录。','返回 bool。'),
 'is_symlink':('判断元数据是否为符号链接。','返回 bool；跟随链接得到的元数据描述目标对象。'),
 'same_file':('比较两份元数据是否标识同一文件。','返回 bool。'),
 'modified':('取得修改时间。','返回时间信息的 expected 值；平台不支持或查询失败时返回错误。'),
 'accessed':('取得访问时间。','返回时间信息的 expected 值。'),
 'read_dir':('打开流式目录枚举。','等待后返回 expected<ReadDir>。'),
 'symlink_metadata':('查询路径自身元数据，不跟随末尾符号链接。','等待后返回 expected<Metadata>。'),
 'canonicalize':('解析路径并取得规范化绝对路径。','等待后返回 expected<path>。'),
 'rename':('重命名或移动路径。','等待后返回 expected<void>。','跨文件系统及覆盖语义由平台决定，失败可能已经产生可观察副作用。'),
 'remove_file':('删除一个文件或符号链接。','等待后返回 expected<void>。'),
 'hard_link':('创建指向现有文件的硬链接。','等待后返回 expected<void>。'),
 'symlink':('创建符号链接。','等待后返回 expected<void>。'),
 'create_dir':('创建单层目录。','等待后返回 expected<void>。'),
 'create_dir_all':('递归创建缺失的目录层级。','等待后返回 expected<void>；目标已经是目录时可成功。'),
 'remove_dir':('删除空目录。','等待后返回 expected<void>。','目录非空时失败。'),
 'remove_dir_all':('依据选项递归删除目录树。','等待后返回 expected<uint64_t>，表示删除数量。','不能把失败解释为没有删除任何条目；应检查错误与部分进度。'),
 'read_to_string':('读取字节并追加或返回字符串。','文件便捷函数返回 expected<string>；流算法返回本次追加字节数的 expected 值。','此接口处理字节，不替应用校验字符编码。'),
 'read_to_end':('向输出容器追加读取，直到 EOF 或达到上限。','等待后返回 expected<size_t>，表示本次追加量。'),
 'copy':('复制文件内容或从 reader 复制到 writer。','文件复制返回字节数的 expected 值；流复制返回 expected<uint64_t>。','流复制保持固定工作缓冲大小，不把无限输入全部存入内存。'),
 'copy_bidirectional':('在两个流之间同时执行双向复制。','等待后返回两个方向传输量的 expected 结果。'),
 'sleep':('挂起当前协程指定时长。','等待后无业务返回值。','非正时长可立即完成；任务取消通过异常传播。'),
 'sleep_until':('挂起到 steady_clock 的指定时间点。','等待后无业务返回值。'),
 'interval':('创建周期定时器。','返回以 auto 保存的周期对象；首次 tick 在一个 period 之后。','返回对象的逐项接口见 @ref periodic_timer 。'),
 'interval_at':('从指定起点创建周期定时器。','返回周期对象；首次 tick 在 start + period 触发。'),
 'timeout':('为受支持的 I/O 操作设置相对截止时间。','保留操作的值类别；co_await 后得到原 I/O 操作的结果。','截止时间从调用 timeout 时计算，不是在开始等待时重新计时。'),
 'timeout_at':('为受支持的 I/O 操作设置绝对截止时间。','保留操作的值类别；等待结果与原操作相同。','仅接受受支持的 I/O 操作，任意 task 竞速使用 select。'),
 'try_lock':('立即尝试取得互斥锁。','取得时返回 true，否则返回 false，不挂起。'),
 'lock':('等待取得互斥锁。','co_await 成功后已持锁，无业务返回值。','手动锁定后必须保证所有退出路径都 unlock，或改用 scoped_lock。'),
 'unlock':('归还互斥许可并交接给等待者。','无返回值。','只能由持有许可的执行路径调用，不能重复归还。'),
 'scoped_lock':('等待取得锁并返回 RAII 守卫。','co_await 后得到 guard，离开作用域自动 unlock。'),
 'try_acquire':('立即尝试取得一个信号量许可。','成功返回 true，许可不足返回 false。'),
 'acquire':('异步等待一个信号量许可。','co_await 成功后取得许可，无业务返回值。'),
 'acquire_permit':('异步等待许可并返回 RAII 守卫。','co_await 后得到 permit，析构归还许可。'),
 'release':('归还指定数量的许可或释放原生所有权。','信号量无返回值；原生拥有者返回转出的描述符。'),
 'available_permits':('读取当前可用许可数量。','返回当前数量快照，不预留许可。'),
 'notify_one':('通知一个条件变量等待者重新检查谓词。','无返回值，不转移互斥锁所有权。'),
 'notify_all':('通知所有条件变量等待者重新检查谓词。','无返回值。'),
 'count_down':('减少闩的剩余计数，归零后唤醒等待者。','无返回值。','不应把计数减少到零以下。'),
 'try_wait':('立即查询闩是否已经归零。','归零返回 true，否则返回 false。'),
 'arrive_and_wait':('到达本轮屏障并等待其他参与者。','co_await 后进入下一轮，无业务返回值。'),
 'make':('创建固定容量 MPSC 通道。','返回 pair<sender,receiver>。','容量必须大于零；发送端可复制，接收端移动独占。'),
 'try_send':('立即尝试发送或将值入队。','通道返回 expected<bool>：满为 false，关闭为错误；网络重载返回传输量。'),
 'try_recv':('立即尝试接收或出队。','通道返回 expected<optional<T>>：空 optional 表示暂时无消息；网络返回传输量。'),
 'data':('取得缓冲区存储的指针。','返回借用指针，不延长缓冲区生命周期。'),
 'size':('取得已初始化或有效范围的长度。','返回 size_t 字节数。'),
 'capacity':('取得缓冲区全部可用容量。','返回 size_t 字节数。'),
 'empty':('检查内容是否为空，或创建立即 EOF 的空读取器。','成员返回 bool；io::empty() 返回空读取器。'),
 'bytes':('取得已初始化字节的借用视图。','返回 span，不包含未初始化尾部。'),
 'span':('取得已初始化字节的借用视图。','返回 span，语义与 bytes() 一致。'),
 'writable_bytes':('取得缓冲区全部可写空间。','返回 span<char>；写入后用 set_size 发布有效范围。'),
 'mutable_bytes':('取得全部可写空间。','返回 span<char>，与 writable_bytes 相同。'),
 'set_size':('标记已经初始化的有效长度。','无返回值。','长度超过 capacity 时抛 out_of_range。'),
 'resize':('调整有效长度，增长部分清零。','无返回值；必要时扩容。'),
 'reserve':('确保至少具有指定容量。','无返回值；保留已初始化内容。'),
 'filled':('取得读缓冲中已经初始化的范围。','返回 span<const char>。'),
 'unfilled':('取得读缓冲尚可写入的尾部。','返回 span<char>。'),
 'advance':('推进读缓冲已经初始化的范围。','无返回值；超出可写空间时失败。'),
 'clear':('将有效范围清空。','无返回值，不一定释放存储容量。'),
 'fill_buf':('获取未消费预读内容，必要时先读取。','等待后返回 expected<span<const char>>。','视图在 consume 或后续读取后失效；底层对象需覆盖借用期。'),
 'consume':('确认消费预读内容的前缀。','无返回值。','数量不能超过已经填充且未消费的范围。'),
 'buffered':('立即查看尚未消费的预读内容。','返回借用的 span<const char>。'),
 'get_ref':('访问被适配器借用的底层对象。','返回 Reader& 或 Writer&。','不要绕过适配器中未消费或未提交的内容。'),
 'pending_bytes':('查看写缓冲中尚未提交的字节。','返回借用的只读 span。'),
 'take_pending':('取走写缓冲中尚未提交的字节。','返回 vector<char>，适配器的待写缓冲随后清空。'),
 'valid':('检查是否关联有效资源或上下文。','返回 bool，不保证上下文尚未停止。'),
 'operator bool':('按布尔条件检查有效性。','返回 bool，与 valid() 一致。'),
 'stopped':('查询上下文是否已进入停止状态。','返回 bool。'),
 'current':('取得当前协程或默认运行时的 I/O 上下文。','返回 io_context 值。'),
 'balanced_context':('选择一个适合负载均衡的上下文。','返回 io_context 值。'),
 'current_context':('取得当前 I/O 上下文。','返回 io_context 值。'),
 'clear_ready':('清除本次观察到的就绪提示。','无返回值；不消费实际数据。'),
 'take':('为读取器增加最大读取量限制。','返回借用底层 reader 的读取适配器。'),
 'chain':('串接两个读取器，前者 EOF 后继续读取后者。','返回组合读取适配器。'),
 'read_until':('读取到分隔符并追加到输出容器。','等待后返回 expected<size_t>，表示本次追加量。'),
 'read_line':('读取一行并追加到字符串。','等待后返回 expected<size_t>，表示本次追加字节数。'),
 'lines':('为读取器创建逐行异步迭代适配器。','返回行读取适配器。'),
 'split_by_delimiter':('按分隔符分段读取。','返回分段异步读取适配器。'),
 'read_integer':('按指定字节序读取整数。','等待后返回 expected 整数结果，提前 EOF 失败。'),
 'write_integer':('按指定字节序写出整数。','等待后返回 expected<void>。'),
 'repeat':('创建重复生成同一字节的读取器。','返回读取器；使用 take 或显式上限限制读取量。'),
 'sink':('创建丢弃所有输入的写入器。','返回写入器，成功写入量等于传入长度。'),
 'duplex':('创建有界内存双工流。','返回一对可读写端点。'),
 'transferred':('读取错误发生前已经生效的传输量。','返回 uint64_t 字节数。'),
 'progress':('读取已完成传输量。','返回 uint64_t，与 transferred() 一致。'),
 'domain':('读取错误所属来源。','返回 error_domain，不应统一按 errno 解释。'),
 'native_code':('读取原生错误码。','返回 int64_t。'),
 'value':('读取错误码或权限数值。','返回当前对象的值，具体类型见签名。'),
 'message':('读取可读错误说明。','返回 string_view，视图不拥有字符串。'),
 'logger':('获取 faio 独立的 spdlog 日志实例。','返回 const shared_ptr<spdlog::logger>&；默认 info 级别，输出到 stderr。','并发记录前完成 sink 配置，不要并发修改 sink 容器。'),
}

PARAMETERS = {
 'other':'被复制、移动或比较的同类型对象；移动后源对象按该类型约定变空。',
 'child':'移动提交的惰性子任务；提交后调用方不再拥有它。',
 't':'移动传入的根协程任务。', 'children':'移动传入的子任务参数包或任务集合，结果顺序对应输入顺序。',
 'body':'接收 scope_context& 的回调，返回本作用域的 task。',
 'function':'执行所需工作的可调用对象；阻塞任务按值保存，非阻塞 I/O 回调不得阻塞。',
 'f':'需要调用的函数对象。', 'priority':'要设置的 task_priority 元数据。',
 'options':'本次操作的选项值，默认值保持当前接口声明所示行为。',
 'context':'目标 I/O 上下文；省略时使用当前上下文。资源创建后不会因协程迁移而改变归属。',
 'address':'目标 SocketAddr；对 send_to 是接收方，对 bind 是本地绑定地址。',
 'addresses':'按输入次序尝试的地址集合，至少应有一个有效地址。',
 'endpoint':'按值保存的 HostPort，包含主机名和服务名/端口。',
 'backlog':'内核待接纳连接队列的长度提示，默认 SOMAXCONN。',
 'target':'接纳后新连接使用的目标 I/O 上下文。',
 'maximum':'本次批量处理的最大消息或连接数；不是全局容量。',
 'native':'需要转移所有权的原生资源拥有者。',
 'interest':'需要观察或操作的读写方向集合。',
 'direction':'要关闭的读、写或双向传输方向。',
 'host':'主机名或数字 IPv4/IPv6 字符串，按值保存。',
 'service':'服务名或数字端口字符串，按值保存。',
 'port':'网络端口号，取值范围由 uint16_t 限定。',
 'socket_type':'解析结果的套接字类型提示，例如 SOCK_STREAM。',
 'flags':'原生操作标志位；默认值和平台支持以当前重载为准。',
 'vectors':'分散/聚集的缓冲区描述符；描述符及引用数据在操作完成前保持有效。',
 'buffers':'缓冲区参数包或集合，每个引用的内存都必须保留到操作完成。',
 'messages':'本批次的数据报描述集合；消息负载在完成前保持有效。',
 'control':'接收或发送辅助信息的控制缓冲区，格式受平台限制。',
 'filename':'要访问的文件或目录路径，按值保存到异步操作。',
 'name':'目标文件或目录路径。', 'source':'要读取、复制、重命名或建立链接的源路径。',
 'destination':'复制、重命名或创建链接的目标路径。', 'from':'源路径。', 'to':'目标路径。',
 'offset':'从文件起始位置计算的非负字节偏移，不推进逻辑游标。',
 'length':'所需的字节长度或文件新长度，以当前函数行为为准。',
 'permissions':'希望设置的文件系统权限值。',
 'contents':'要写入文件的字节；借用输入在完成前保持有效，拥有型输入移交所有权。',
 'max_bytes':'本次读取或追加的最大字节数，用于约束内存与无限流。',
 'limit':'最大允许读取的字节数。', 'reader':'借用的异步读取器，必须覆盖组合操作的生命周期。',
 'writer':'借用的异步写入器，必须覆盖组合操作的生命周期。',
 'output':'接收追加结果的容器；操作未完成前不能销毁或被其他任务无序修改。',
 'buffer_size':'流复制使用的固定工作缓冲容量，必须大于零。',
 'first':'第一个读取器或双工流。', 'second':'第二个读取器或双工流。',
 'stream':'需要借用、适配或拆分的异步字节流。',
 'delimiter':'用来终止或划分读取内容的字节分隔符。',
 'order':'整数的读取或写入字节序。',
 'byte':'重复生成的单个字节值。',
 'duration':'等待时长或 socket linger 时长；时间单位由参数类型给出。',
 'expired_time':'steady_clock 的绝对到期时间。',
 'period':'周期长度，必须使用有效的正时长。', 'start':'steady_clock 的周期起点；首次触发在 start + period。',
 'operation':'受支持的 I/O 操作；右值移入返回对象，左值保留借用身份。',
 'deadline':'steady_clock 的绝对截止时间。', 'interval':'从调用 timeout 时开始计算的相对毫秒时长。',
 'm':'已经取得或需要协调的互斥锁对象，覆盖整个等待过程。',
 'predicate':'在锁保护下判断共享状态是否满足条件的谓词。',
 'pred':'在锁保护下检查的共享状态谓词。',
 'initialized':'已经初始化的有效字节数，不得超过提供的存储空间。',
 'storage':'借用的原始字节存储空间，在视图和异步操作期间保持有效。',
 'view':'借用的字节视图，视图不延长内存生命周期。',
 'owner':'移动传入的原生描述符拥有者，成功后所有权交给包装对象。',
 'descriptor':'原生文件描述符；是否移交所有权由当前方法说明决定。',
 'fd':'需要接管关闭责任的原生 socket 或文件描述符。',
 'policy':'运行时关闭策略，默认 drain；常驻任务仍需有退出路径。',
 'err_code':'错误码；与 domain 一起解释，不能把所有来源都视为 errno。',
 'transferred':'错误发生前已经生效的字节数。', 'domain':'原生错误的来源标识。',
 'ip':'IP 地址的值对象。', 'scope_id':'IPv6 作用域编号，用于链路本地等地址。',
 'text':'需要解析的数字地址字符串。',
}

CONFIG = {
 'set_mode':('选择单线程或多线程调度模式。','selected','运行模式 current_thread 或 multi_thread。'),
 'set_io_backend':('校验所请求的 Linux 后端与编译结果一致。','selected','IO_EPOLL 或 IO_URING；必须与编译时固定的后端相同。'),
 'set_num_workers':('设置多线程 worker 数量。','num_workers','工作线程数量；current_thread 配置只使用一个驱动线程。'),
 'set_num_events':('设置单次 I/O 事件批量大小。','num_events','批量大小，实际限制为 1 到 256，不代表全局在途请求上限。'),
 'set_filesystem_threads':('设置独立文件服务的线程数量。','count','处理回退文件操作的线程数量。'),
 'set_resolver_threads':('设置独立 DNS 服务的线程数量。','count','处理主机名解析的线程数量。'),
 'set_filesystem_queue_limit':('设置文件服务的排队容量。','count','文件操作服务允许排队的请求数量。'),
 'set_resolver_queue_limit':('设置 DNS 服务的排队容量。','count','名称解析服务允许排队的请求数量。'),
 'set_blocking_queue_limit':('设置用户阻塞池的排队容量。','count','spawn_blocking 允许排队的任务数量。'),
 'set_max_blocking_threads':('设置用户阻塞池最大线程数量。','count','阻塞线程数上限，独立于协程 worker。'),
 'set_blocking_keep_alive':('设置阻塞线程空闲保活时间。','duration','空闲后保留线程的毫秒时长。'),
 'set_io_interval':('设置持续执行任务时轮询 I/O 的间隔。','io_interval','相邻 I/O 轮询之间允许执行的调度步数。'),
 'set_global_queue_interval':('设置查询全局队列的公平性间隔。','global_queue_interval','在本地任务持续存在时检查全局队列的步数间隔。'),
 'set_idle_spin_count':('设置休眠前的有限轮询次数。','idle_spin_count','空闲轮询次数；0 表示更快进入休眠。'),
 'set_submit_interval':('保存提交间隔配置。','submit_interval','保留的批量提交间隔；当前 epoll/kqueue 请求在等待时直接尝试操作。'),
}

SOCKET_OPTIONS = {
 'nodelay':'TCP_NODELAY，禁用或查询 Nagle 算法', 'keepalive':'SO_KEEPALIVE 保活',
 'recv_buffer_size':'接收缓冲区字节容量', 'send_buffer_size':'发送缓冲区字节容量',
 'linger':'关闭时等待发送完成的 linger 时长', 'reuseaddr':'本地地址复用',
 'reuseport':'端口复用（受平台支持限制）', 'broadcast':'UDP 广播', 'only_v6':'仅接受 IPv6',
 'ttl':'IP 生存期/跳数', 'multicast_loop_v4':'IPv4 组播回环', 'multicast_loop_v6':'IPv6 组播回环',
 'multicast_ttl_v4':'IPv4 组播 TTL', 'multicast_hops_v6':'IPv6 组播跳数',
 'multicast_interface_v4':'IPv4 组播接口', 'multicast_interface_v6':'IPv6 组播接口',
 'recv_packet_info_v4':'接收 IPv4 分组信息', 'recv_packet_info_v6':'接收 IPv6 分组信息',
 'passcred':'Unix 凭据传递', 'mark':'Linux socket 标记',
}

METHODS.update({
 'is_readable':('检查是否观察到读就绪。','返回 bool。'),
 'is_writable':('检查是否观察到写就绪。','返回 bool。'),
 'is_error':('检查是否观察到错误提示。','返回 bool；错误细节需通过实际 I/O 或 take_error 查询。'),
 'is_read_closed':('检查读方向是否已关闭。','返回 bool；实际 read 仍可能先返回未消费的数据。'),
 'is_write_closed':('检查写方向是否已关闭。','返回 bool。'),
 'recursive':('设置是否创建缺失的父目录。','返回 DirBuilder&，可继续配置。'),
 'bits':('取得保存的文件权限位。','返回 std::filesystem::perms。'),
 'readonly':('检查所有写权限位是否均未设置。','没有 owner/group/others 写权限时返回 true。'),
 'set_readonly':('修改权限值中的写权限位。','无返回值；true 清除全部写位，false 添加 owner_write。','仅修改值对象；写回文件需调用 set_permissions。'),
 'start':('从文件开头计算绝对定位。','返回 seek_from；超过 int64_t 范围时抛 out_of_range。'),
 'end':('从文件末尾计算相对定位。','返回 seek_from，允许负位移。'),
 'into_inner':('释放异步包装并归还原生拥有者。','返回 expected<Owner>；在途操作导致导出失败时返回错误。'),
 'nonblocking':('查询描述符的非阻塞标志。','返回 expected<bool>。'),
 'set_nonblocking':('设置描述符的非阻塞标志。','返回 expected<void>。','异步就绪操作要求非阻塞模式；不能在在途操作中切换为阻塞模式。'),
 'take_fd':('尝试导出并放弃底层描述符所有权。','返回描述符；导出失败时返回无效描述符。','调用者负责关闭成功导出的描述符；优先用可报告错误的 into_native。'),
 'addr':('借用地址的原生表示。','IPv4 返回网络字节序 uint32_t；IPv6 返回 const in6_addr&。'),
 'is_loopback':('判断是否为回环地址。','是回环地址时返回 true。'),
 'is_multicast':('判断是否为组播地址。','是组播地址时返回 true。'),
 'is_unspecified':('判断是否为未指定地址。','IPv4 0.0.0.0 或 IPv6 :: 时返回 true。'),
 'octets':('以网络顺序取得四个 IPv4 字节。','返回 array<uint8_t,4>。'),
 'parse':('从文本解析数字 IP 或 IP:port 端点。','返回对应地址类型的 expected 结果；非法格式返回错误。','不执行 DNS 查询；名称解析使用 lookup_host。IPv6 带端口时使用 [::1]:8080 形式。'),
 'numeric_parse':('解析数字主机地址与端口。','返回 expected<SocketAddr>，不会发起主机名查询。'),
 'to_string':('把地址格式化为字符串。','返回拥有型 string；SocketAddr 包含端口，IPv6 使用方括号。'),
 'family':('取得原生地址协议族。','返回 AF_INET 或 AF_INET6。'),
 'ip':('取得端点的 IP 部分。','返回 variant<Ipv4Addr,Ipv6Addr>。'),
 'is_ipv4':('检查端点是否使用 IPv4。','返回 bool。'),
 'is_ipv6':('检查端点是否使用 IPv6。','返回 bool。'),
 'length':('取得原生 sockaddr 的实际长度。','返回 socklen_t，供原生系统调用使用。'),
 'port':('取得端点的端口。','返回主机字节序 uint16_t。'),
 'scope_id':('取得 IPv6 scope id。','返回 uint32_t；IPv4 无 scope id。'),
 'set_ip':('替换端点的 IP 地址。','无返回值。'),
 'set_port':('替换端点的端口。','无返回值；传入主机字节序端口。'),
 'set_scope_id':('设置 IPv6 scope id。','无返回值。'),
 'sockaddr':('借用端点的原生 sockaddr 存储。','返回借用指针，生命周期不超过当前地址对象。'),
 'take_error':('读取并清除套接字的待处理错误。','返回 expected<int>；成功值 0 表示没有待处理错误。'),
})
PARAMETERS.update({
 'bits':'保存的文件权限位；默认 unknown。',
 'enabled':'true 启用该选项，false 关闭。',
 'read_capacity':'读侧预读缓冲容量，单位为字节，必须大于零。',
 'write_capacity':'写侧缓冲容量，单位为字节，必须大于零。',
 'hostname':'主机名或数字 IP，复制保存到对象。',
 'service_name':'服务名或端口字符串，复制保存到对象。',
 'network_order':'网络字节序的 IPv4 地址整数。',
 'scope':'IPv6 scope id；通常用于链路本地地址。',
 'ipv6':'true 创建 IPv6 socket，false 创建 IPv4 socket。',
 'participants':'每轮预期到达的参与者数量。',
 'n':'本次减少的剩余计数，不得超过当前计数。',
 'a':'地址的第一个字节或 16 位分段。', 'b':'地址的第二个字节或 16 位分段。',
 'c':'地址的第三个字节或 16 位分段。', 'd':'地址的第四个字节或 16 位分段。',
 'e':'IPv6 地址的第五个 16 位分段。', 'g':'IPv6 地址的第七个 16 位分段。',
 'h':'IPv6 地址的第八个 16 位分段。',
})

MISSING = set()


def documentation(spec, name, parameters, result_type, member):
    public = spec['public']
    short = public.split('::')[-1]
    entry = METHODS.get(name)
    params = dict(PARAMETERS)
    note = ''
    if short == 'config_builder' and name in CONFIG:
        brief, parameter, description = CONFIG[name]
        entry = (brief,'返回当前 config_builder&，支持链式配置。','最终配置合法性在运行时配置入口验证。')
        params[parameter] = description
    option = name.removeprefix('set_')
    if spec['group'] == 'network' and option in SOCKET_OPTIONS:
        label = SOCKET_OPTIONS[option]
        entry = ('设置'+label+'。' if name.startswith('set_') else '读取'+label+'。',
                 '返回 expected<void>。' if name.startswith('set_') else '返回该选项值的 expected 结果。',
                 '配置的含义与可用性受操作系统及套接字类型限制；失败返回系统错误。')
        params.update(on='true 启用，false 关闭该 socket 选项。',size='缓冲容量，单位为字节。',value='此选项的整数值，合法范围由平台确定。',network_interface='目标网络接口的地址或索引。',ttl='组播分组的 TTL。',mark='Linux socket 标记值。')
    if name.startswith(('join_multicast_', 'leave_multicast_')):
        entry=('加入组播组。' if name.startswith('join') else '离开组播组。','返回 expected<void>。','组播地址与网络接口须匹配对应 IP 协议族。')
        params.update(group='要加入或离开的组播 IP 地址。',network_interface='用于组播的网络接口地址或索引。')
    if name.startswith('try_') and entry is None:
        target = name[4:]
        if target in METHODS:
            base = METHODS[target]
            entry=('立即尝试：'+base[0],base[1].replace('等待后','调用后'),'不会挂起；暂不可用时返回 would-block 错误，调用者可等待就绪后重试。')
    if name == short:
        entry=('构造'+short+'对象。','构造函数不返回值。',spec.get('note','对象的复制、移动和资源所有权遵循类型说明。'))
        if short in ('semaphore','latch','barrier'):
            params.update(permits='初始可用许可数量。',count='初始参与者或剩余计数。',expected='每轮必须到达的参与者数量。')
    if public == 'faio::io' and name == 'write_all_buf':
        entry=('接管拥有型缓冲区并写出全部有效字节。','等待后返回 expected<io_transfer>；成功时 buffer 归还原缓冲，bytes 为总写出量。','失败通过 error().transferred() 报告部分进度。')
    if short == 'File' and name == 'read_exact':
        entry=('反复读取，直到填满整个缓冲区。','等待后返回 expected<size_t>；成功值等于缓冲长度。','提前 EOF 返回 UnexpectedEOF；error().transferred() 保留部分进度。')
    if short == 'DirBuilder' and name == 'create':
        entry=('按构建器的递归与权限选项创建目录。','等待后返回 expected<void>。')
    if short == 'io_buffer' and name == 'copy':
        entry=('复制输入字节，创建独占缓冲区。','返回 io_buffer；size 和 capacity 均为输入长度。','复制完成后不再借用输入。')
    if short == 'OwnedFd' and name == 'get':
        entry=('借用当前 POSIX 描述符。','返回 int，不转移关闭责任；空对象为 -1。')
    if short == 'AsyncFd' and name == 'create':
        entry=('接管原生拥有者并注册非阻塞异步描述符。','返回 expected<AsyncFd>，失败报告系统错误或 ENOMEM。','不接受普通文件、目录或块设备。')
    if short == 'AsyncFd' and name in ('ready','readable','writable'):
        entry=('等待所需方向的就绪提示。','等待后返回 expected<AsyncFdReadyGuard>。','守卫不拥有方向执行权；使用 try_io 执行非阻塞回调。')
    if short == 'AsyncFdReadyGuard' and name == 'ready':
        entry=('取得本次观察的就绪信息。','直接返回 Ready 值，不进行新的等待。')
    if short == 'scope_context' and name == 'spawn':
        entry=('提交由当前 scope 跟踪的子任务。','无返回值；scope 退出前等待子任务全部结束。')
    if short == 'owned_native_socket' and name in ('get','reset','release','operator bool'):
        entry={'get':('借用当前原生 socket。','返回原生句柄，不转移所有权。'),'reset':('关闭当前 socket 并置空。','无返回值。'),'release':('转移当前 socket 的关闭责任。','返回原生句柄；原拥有者变为空。'),'operator bool':('检查是否持有有效 socket。','有效时返回 true。')}[name]
    if short == 'seek_from' and name == 'current':
        entry=('从当前文件游标计算相对定位。','返回 seek_from，允许负位移。')
    if short == 'Metadata' and name in ('modified','accessed'):
        entry=('取得元数据快照中的'+('修改' if name == 'modified' else '访问')+'时间。','返回 system_clock::time_point；不执行新的系统查询。')
    if short == 'SocketAddr' and name == 'capacity':
        entry=('取得地址存储可容纳的最大原生长度。','返回 socklen_t。')
    if short == 'Ipv6Addr' and name == 'Ipv6Addr':
        params['f']='IPv6 地址的第六个 16 位分段。'
    if short == 'OpenOptions' and name in ('read','write','append','truncate','create','create_new','creation_permissions'):
        meanings={'read':'允许读取','write':'允许写入','append':'每次写入追加到文件尾','truncate':'打开已有文件时截断内容','create':'不存在时创建文件','create_new':'仅创建新文件，已存在时失败','creation_permissions':'设置新建文件的访问权限'}
        entry=(meanings[name]+'。','返回 OpenOptions&，可继续组合其他打开选项。','创建、追加或截断须启用写入；不合法组合返回错误。')
        params['enabled']='true 启用该打开选项，false 关闭。'
    if short == 'sender' and name == 'send':
        entry=('在通道中发送一个值，满队列时挂起。','等待后返回 expected<void>；接收端关闭时返回 ClosedChannel。','传入值按移动语义存入队列。')
    if short == 'receiver' and name == 'recv':
        entry=('从通道接收一个值，空队列时挂起。','等待后返回 expected<T>；发送端全部关闭且队列排空时返回 ClosedChannel。','同一接收端只能有一个未完成的 recv。')
    if spec['group']=='filesystem' and public=='faio::fs' and name in ('read','write','copy'):
        entry={
          'read':('读取整个文件或受限前缀。','等待后返回 expected<vector<char>>。','limit/max_bytes 限制读取长度。'),
          'write':('创建或覆盖整个文件的内容。','等待后返回 expected<void>。','已有内容被覆盖；借用输入须保留到操作完成。'),
          'copy':('复制文件内容到目标路径。','等待后返回 expected<uint64_t>，表示复制字节数。','失败不保证目标未被修改。'),
        }[name]
    if entry is None:
        MISSING.add(public+'::'+name)
        entry=('UNRESOLVED '+name,'UNRESOLVED return')
    # Describe the current overload rather than alternatives on other types.
    if name == 'connect' and short in ('TcpStream','TcpSocket','UdpSocket'):
        entry=(entry[0], '等待后返回 expected<void>，socket 保留当前包装。' if short == 'UdpSocket' else '等待后返回 expected<TcpStream>，成功值拥有连接。', *entry[2:])
    if name in ('read','write') and spec['group'] in ('network','filesystem','io') and short not in ('OpenOptions',):
        owned = any('io_buffer' in p[0] for p in parameters)
        entry=(entry[0], '等待后返回 expected<io_transfer>；buffer 归还拥有的缓冲，bytes 为本次传输量。' if owned else '等待后返回 expected<size_t>，表示本次实际传输字节数。', *entry[2:])
    brief, returns, *rest = entry
    note = rest[0] if rest else note
    param_docs = {}
    for kind, parameter, default in parameters:
        if parameter in ('buffer','bytes','input'):
            if 'io_buffer' in kind:
                description='移动传入的拥有型缓冲区；写入使用有效范围，读取使用可写容量，完成后通过 io_transfer 归还。'
            elif 'const' in kind or 'string_view' in kind or 'shared_const_buffer' in kind:
                description='要写入的只读字节视图；借用负载在操作完成前保持有效且不得并发修改。'
            else:
                description='可写字节缓冲区；在操作完成前保持内存与地址有效，不能被其他任务无序修改。'
        elif parameter == 'capacity':
            description='缓冲区或通道的容量；缓冲适配器和 MPSC 通道要求大于零。'
        elif parameter == 'count':
            description={'count_down':'本次减少的剩余计数，不得超过尚未完成的数量。','release':'本次归还的信号量许可数量。','consume':'确认消费的字节数，不得超过已填充内容。','advance':'本次新增的已初始化字节数，不得超过未填充范围。'}.get(name,'本次操作的字节数，不得超过提供缓冲的有效容量。')
        elif parameter == 'offset' and short == 'seek_from':
            description='相对所选起点的字节位移；current/end 支持负值，start 要求非负且不超 int64_t。'
        elif parameter == 'endpoint' and 'string_view' in kind:
            description='数字 IP 与端口的文本，如 127.0.0.1:8080 或 [::1]:8080。'
        elif parameter == 'value':
            description='要设置、编码或移动发送的值；具体类型见当前重载签名。'
        elif parameter == 'size':
            description='目标字节长度；set_size 不得超过 capacity，resize 可扩容。'
        elif parameter in params:
            description=params[parameter]
        else:
            MISSING.add(public+'::'+name+' parameter '+parameter+' ('+kind+')')
            description='UNRESOLVED '+parameter
        if default:
            description += ' 默认值：`'+default+'`。'
        param_docs[parameter]=description
    return {'brief':brief,'returns':returns,'params':param_docs,'note':note,
            'errors':('返回 expected 的重载以 error() 报告失败；先检查成功状态再读取值。任务中的异常沿等待链传播。' if 'expected' in result_type or result_type in ('auto','decltype(auto)') else '')}
