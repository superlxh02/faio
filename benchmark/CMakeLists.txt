# -----------------------------------------------------------------------------
# 第三方依赖：Asio 是可选对照，线程库供独立负载发生器使用
# -----------------------------------------------------------------------------
# faio_find_asio() 尝试提供 asio::asio；未找到时只跳过 Asio 服务目标。
# faio 的 TCP/协程基准不依赖 Asio。Tokio 由其 Rust/Cargo 工程构建，本文件
# 不建立 Tokio CMake 目标，faio 与 Tokio 的横向比较也不要求安装 Asio。
# spdlog 已由根目录提供，faio::faio 传播它；独立 Asio 目标需要显式链接。
faio_find_asio()
find_package(Threads REQUIRED)

# -----------------------------------------------------------------------------
# faio TCP 服务：继承库的依赖、平台宏和编译器配置
# -----------------------------------------------------------------------------
# Windows 的 ws2_32、mswsock、iphlpapi 由 faio::faio 传播，无需在此重复链接。
# MSVC/clang-cl 的 ABI 参数与 MinGW/GNU 风格参数均由根目录统一选择。
add_executable(faio_tcp_benchmark tcp/faio_tcp_benchmark.cpp)
target_link_libraries(faio_tcp_benchmark PRIVATE faio::faio)

# -----------------------------------------------------------------------------
# Asio TCP 服务：可选第三方目标，自行声明平台与编译器参数
# -----------------------------------------------------------------------------
if(TARGET asio::asio)
    add_executable(asio_tcp_benchmark tcp/asio_tcp_benchmark.cpp)
    target_include_directories(asio_tcp_benchmark PRIVATE ../include)
    target_link_libraries(asio_tcp_benchmark PRIVATE
        asio::asio          # standalone Asio 的头文件目标。
        Threads::Threads    # CMake 为当前平台选择的线程库。
        spdlog::spdlog)     # 独立服务直接使用日志库，不通过 faio 传递。

    # 此目标不链接 faio::faio，因此 Windows socket 库及平台宏需独立设置。
    # ws2_32 提供 Winsock API，mswsock 提供 Windows socket 扩展支持。
    if(WIN32)
        target_link_libraries(asio_tcp_benchmark PRIVATE ws2_32 mswsock)
        target_compile_definitions(asio_tcp_benchmark PRIVATE
            NOMINMAX                # 禁止 Windows min/max 宏污染 C++ 名称。
            WIN32_LEAN_AND_MEAN      # 减少 Windows 头文件的声明范围。
            _WIN32_WINNT=0x0A00)     # 选择与 faio 相同的 Windows 10 API 基线。

        # MSVC 是 CMake 对 MSVC ABI 工具链的判定，也覆盖 clang-cl；不能只按
        # CMAKE_CXX_COMPILER_ID 是否为 MSVC 决定选项，否则会漏掉 clang-cl。
        # MinGW 使用 GNU 命令行，不接收这些斜线选项，保留其默认编译配置。
        if(MSVC)
            target_compile_options(asio_tcp_benchmark PRIVATE
                /utf-8           # MSVC/clang-cl：源码与执行字符集使用 UTF-8。
                /permissive-)    # MSVC/clang-cl：启用标准一致的语言规则。
        endif()
    endif()
endif()

# -----------------------------------------------------------------------------
# 协程基准：优化基线由根目录与工具链配置决定
# -----------------------------------------------------------------------------
# 保留原 GCC -O2 对照基线；Rust profile.release 也采用 opt-level=2。
# Windows 目标从 faio::faio 继承各编译器选项，不额外追加 GNU 风格警告；
# 非 Windows 目标继续显式保留原 -Wall/-Wextra/-Wpedantic 参数。
add_executable(faio_coro_benchmark coro/faio_coro_benchmark.cpp)
target_link_libraries(faio_coro_benchmark PRIVATE faio::faio)
if(NOT WIN32)
    target_compile_options(faio_coro_benchmark PRIVATE -Wall -Wextra -Wpedantic)
endif()

# 单线程运行时基准保持原有平台范围，仅在 Linux/macOS 等非 Windows 平台建立。
if(NOT WIN32)
    add_executable(faio_current_thread_benchmark coro/current_thread_benchmark.cpp)
    target_link_libraries(faio_current_thread_benchmark PRIVATE faio::faio)
    target_compile_options(faio_current_thread_benchmark PRIVATE -Wall -Wextra -Wpedantic)
endif()

# -----------------------------------------------------------------------------
# 独立 TCP 负载发生器：同一个客户端测量 faio/Tokio/可选 Asio 服务
# -----------------------------------------------------------------------------
# 客户端不链接 faio 或 Asio，避免客户端运行时实现影响服务端横向比较。
# 显式要求 C++23 并链接线程库；仅 Windows 分支增加真正需要的 Winsock 库。
add_executable(tcp_echo_load tcp/tcp_echo_load.cpp)
target_compile_features(tcp_echo_load PRIVATE cxx_std_23)
target_link_libraries(tcp_echo_load PRIVATE Threads::Threads)

if(WIN32)
    # 负载发生器直接调用 Winsock，不使用 mswsock 扩展，因此只链接 ws2_32。
    target_link_libraries(tcp_echo_load PRIVATE ws2_32)
    target_compile_definitions(tcp_echo_load PRIVATE
        NOMINMAX                # 禁止 min/max 宏，保持 std::min/std::max 可用。
        WIN32_LEAN_AND_MEAN      # 避免引入无关的 Windows 头文件声明。
        _WIN32_WINNT=0x0A00)     # 与服务目标保持同一 Windows 10 API 声明范围。

    # MSVC 和 clang-cl 共用 Microsoft ABI；MinGW 不追加这组斜线参数。
    if(MSVC)
        target_compile_options(tcp_echo_load PRIVATE
            /utf-8           # MSVC/clang-cl：统一 UTF-8 源码及执行字符集。
            /W4              # MSVC/clang-cl：保持既有第四级警告等级。
            /permissive-)    # MSVC/clang-cl：严格标准一致性模式。
    endif()
endif()
