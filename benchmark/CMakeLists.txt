# Asio 是额外的第三方对照；faio/Tokio 基准不依赖 Asio 的安装。
find_package(asio CONFIG QUIET)
if (NOT TARGET asio::asio)
    find_path(ASIO_INCLUDE_DIR asio.hpp)
    if (ASIO_INCLUDE_DIR)
        add_library(asio::asio INTERFACE IMPORTED)
        set_target_properties(asio::asio PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${ASIO_INCLUDE_DIR}"
                INTERFACE_COMPILE_DEFINITIONS ASIO_STANDALONE)
    endif ()
endif ()
find_package(Threads REQUIRED)

add_executable(faio_tcp_benchmark tcp/faio_tcp_benchmark.cpp)
target_link_libraries(faio_tcp_benchmark PRIVATE faio::faio)

if (TARGET asio::asio)
    add_executable(asio_tcp_benchmark tcp/asio_tcp_benchmark.cpp)
    target_include_directories(asio_tcp_benchmark PRIVATE ../include)
    target_link_libraries(asio_tcp_benchmark PRIVATE asio::asio Threads::Threads spdlog::spdlog)
endif ()

# GCC -O2 对照测试；编译参数由根 CMake 统一，Rust 侧 profile.release 也使用 opt-level=2。
add_executable(faio_coro_benchmark coro/faio_coro_benchmark.cpp)
target_link_libraries(faio_coro_benchmark PRIVATE faio::faio)
target_compile_options(faio_coro_benchmark PRIVATE -Wall -Wextra -Wpedantic)

add_executable(faio_current_thread_benchmark coro/current_thread_benchmark.cpp)
target_link_libraries(faio_current_thread_benchmark PRIVATE faio::faio)
target_compile_options(faio_current_thread_benchmark PRIVATE -Wall -Wextra -Wpedantic)

# 同一原生负载发生器测两个服务，避免客户端实现引入语言差异。
add_executable(tcp_echo_load tcp/tcp_echo_load.cpp)
target_compile_features(tcp_echo_load PRIVATE cxx_std_23)
target_link_libraries(tcp_echo_load PRIVATE Threads::Threads)
