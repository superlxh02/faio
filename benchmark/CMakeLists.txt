find_package(asio CONFIG QUIET)
if(NOT TARGET asio::asio)
    find_path(ASIO_INCLUDE_DIR asio.hpp REQUIRED)
    add_library(asio::asio INTERFACE IMPORTED)
    set_target_properties(asio::asio PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${ASIO_INCLUDE_DIR}"
        INTERFACE_COMPILE_DEFINITIONS ASIO_STANDALONE)
endif()
find_package(Threads REQUIRED)

add_executable(faio_tcp_benchmark tcp/faio_tcp_benchmark.cpp)
target_link_libraries(faio_tcp_benchmark PRIVATE faio::faio)

add_executable(asio_tcp_benchmark tcp/asio_tcp_benchmark.cpp)
target_include_directories(asio_tcp_benchmark PRIVATE ../include)
target_link_libraries(asio_tcp_benchmark PRIVATE asio::asio Threads::Threads spdlog::spdlog)

# GCC -O2 对照测试；编译参数由根 CMake 统一，Rust 侧 profile.release 也使用 opt-level=2。
add_executable(faio_coro_benchmark coro/faio_coro_benchmark.cpp)
target_link_libraries(faio_coro_benchmark PRIVATE faio::faio)
target_compile_options(faio_coro_benchmark PRIVATE -Wall -Wextra -Wpedantic)
