#[[
@file benchmark/CMakeLists.txt
@brief 性能基准目标与对照工具。
]]
faio_find_asio()
find_package(Threads REQUIRED)

add_executable(faio_tcp_benchmark tcp/faio_tcp_benchmark.cpp)
target_link_libraries(faio_tcp_benchmark PRIVATE faio::faio)

if(TARGET asio::asio)
    add_executable(asio_tcp_benchmark tcp/asio_tcp_benchmark.cpp)
    target_include_directories(asio_tcp_benchmark PRIVATE ../include)
    target_link_libraries(asio_tcp_benchmark PRIVATE
        asio::asio
        Threads::Threads
        spdlog::spdlog)

    if(WIN32)
        target_link_libraries(asio_tcp_benchmark PRIVATE ws2_32 mswsock)
        target_compile_definitions(asio_tcp_benchmark PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            _WIN32_WINNT=0x0A00)

        if(MSVC)
            target_compile_options(asio_tcp_benchmark PRIVATE
                /utf-8
                /permissive-)
        endif()
    endif()
endif()

add_executable(faio_coro_benchmark coro/faio_coro_benchmark.cpp)
target_link_libraries(faio_coro_benchmark PRIVATE faio::faio)
add_executable(faio_frame_allocation_benchmark
    coro/frame_allocation_benchmark.cpp coro/frame_allocation_workloads.cpp)
target_link_libraries(faio_frame_allocation_benchmark PRIVATE faio::faio)
set_target_properties(faio_frame_allocation_benchmark PROPERTIES INTERPROCEDURAL_OPTIMIZATION FALSE)
add_executable(faio_frame_allocation_new_benchmark
    coro/frame_allocation_benchmark.cpp coro/frame_allocation_workloads.cpp)
target_link_libraries(faio_frame_allocation_new_benchmark PRIVATE faio::faio)
target_compile_definitions(faio_frame_allocation_new_benchmark PRIVATE FAIO_COROUTINE_FRAME_USE_GLOBAL_NEW=1)
set_target_properties(faio_frame_allocation_new_benchmark PROPERTIES INTERPROCEDURAL_OPTIMIZATION FALSE)
add_executable(faio_io_cpu_benchmark io/faio_io_cpu_benchmark.cpp)
target_link_libraries(faio_io_cpu_benchmark PRIVATE faio::faio)
add_executable(faio_blocking_pool_benchmark io/blocking_pool_benchmark.cpp)
target_link_libraries(faio_blocking_pool_benchmark PRIVATE faio::faio)
add_executable(faio_mpsc_ready_queue_benchmark coro/mpsc_ready_queue_benchmark.cpp)
target_link_libraries(faio_mpsc_ready_queue_benchmark PRIVATE faio::faio)
if(NOT WIN32)
    target_compile_options(faio_coro_benchmark PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(faio_mpsc_ready_queue_benchmark PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(NOT WIN32)
    add_executable(faio_current_thread_benchmark coro/current_thread_benchmark.cpp)
    target_link_libraries(faio_current_thread_benchmark PRIVATE faio::faio)
    target_compile_options(faio_current_thread_benchmark PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(tcp_echo_load tcp/tcp_echo_load.cpp)
target_compile_features(tcp_echo_load PRIVATE cxx_std_23)
target_link_libraries(tcp_echo_load PRIVATE Threads::Threads)

if(WIN32)

    target_link_libraries(tcp_echo_load PRIVATE ws2_32)
    target_compile_definitions(tcp_echo_load PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00)

    if(MSVC)
        target_compile_options(tcp_echo_load PRIVATE
            /utf-8
            /W4
            /permissive-)
    endif()
endif()

if(NOT WIN32)
    add_executable(tcp_connect_load tcp/tcp_connect_load.cpp)
    target_compile_features(tcp_connect_load PRIVATE cxx_std_23)
    target_link_libraries(tcp_connect_load PRIVATE Threads::Threads)
endif()
