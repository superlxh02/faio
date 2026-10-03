# -----------------------------------------------------------------------------
# 公共示例：每个程序使用自己的源码并继承 faio 的依赖与编译配置
# -----------------------------------------------------------------------------
# faio::faio 传播 C++23、Threads 和 spdlog；示例不需要重新查找或直接链接
# 第三方日志库。Windows 的 Winsock/mswsock/iphlpapi 也由该目标统一传播。
# 编译器选项在根目录按 ABI 划分：MSVC 和 clang-cl 继承 MSVC 风格参数，
# MinGW GCC 与其他采用 GNU 命令行的编译器继承原有警告选项。
# 每个示例仅由自己的 cpp 构建，不链接其他示例或公共示例实现。
foreach(example IN ITEMS
    coroutine_basics
    coroutine_sync
    tcp_echo_server
    udp_echo_server
    tcp_counter_server
    blocking_thread_pool
    tcp_echo_server_single_thread
    file_and_directory)
    add_executable(${example} ${example}.cpp)
    target_link_libraries(${example} PRIVATE faio::faio)
endforeach()

# -----------------------------------------------------------------------------
# Windows 专有示例
# -----------------------------------------------------------------------------
# 该示例验证 Windows I/O 框架能力；构建不依赖是否开启 CTest。
if(WIN32)
    add_executable(faio_windows_framework windows_framework.cpp)
    target_link_libraries(faio_windows_framework PRIVATE faio::faio)
endif()

# -----------------------------------------------------------------------------
# 有限示例的 CTest 注册：各平台保留相同名称、参数和超时
# -----------------------------------------------------------------------------
# 只有开启 FAIO_BUILD_TESTS 才注册示例测试。普通 echo server 长期监听，
# 不直接作为有限测试运行；tcp_counter_server 使用已有 --self-test 退出路径。
if(FAIO_BUILD_TESTS)
    enable_testing()
    add_test(NAME coroutine_basics COMMAND coroutine_basics)
    add_test(NAME coroutine_sync COMMAND coroutine_sync)
    add_test(NAME blocking_thread_pool COMMAND blocking_thread_pool)
    add_test(NAME tcp_counter_server COMMAND tcp_counter_server --self-test)

    # 文件示例在自己的构建子目录中运行，生成的 test/main.c 不写进源码目录。
    set(file_example_workdir "${CMAKE_CURRENT_BINARY_DIR}/file_and_directory_workdir")
    file(MAKE_DIRECTORY "${file_example_workdir}")
    add_test(NAME file_and_directory COMMAND file_and_directory)
    set_tests_properties(file_and_directory PROPERTIES
        WORKING_DIRECTORY "${file_example_workdir}")
    set_tests_properties(
        coroutine_basics
        coroutine_sync
        blocking_thread_pool
        tcp_counter_server
        file_and_directory
        PROPERTIES TIMEOUT 30 LABELS examples)

    # Windows 另外注册有限的框架示例，以及三个服务器的真实网络回包检查。
    if(WIN32)
        add_test(NAME faio_windows_framework COMMAND faio_windows_framework)
        set_tests_properties(faio_windows_framework PROPERTIES TIMEOUT 30 LABELS examples)

        # Python 仅用于这个 Windows 网络联测分支，不增加公共示例的运行依赖。
        # 检查程序负责启动服务、发送请求、核对回包并终止服务进程。
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        foreach(server IN ITEMS
            tcp_echo_server
            tcp_echo_server_single_thread
            udp_echo_server)
            if(server STREQUAL "udp_echo_server")
                set(protocol udp)
                set(port 9090)
            else()
                set(protocol tcp)
                set(port 8080)
            endif()

            add_test(NAME ${server}_network_smoke
                COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/scripts/check_windows_servers.py"
                --executable "$<TARGET_FILE:${server}>"
                --protocol ${protocol}
                --port ${port})
            # 两个 TCP 示例使用相同端口；资源锁使这些测试不会并发抢占端口。
            # UDP 测试继续使用同一个既有锁名，保留整个服务器联测组的执行规则。
            set_tests_properties(${server}_network_smoke PROPERTIES
                TIMEOUT 20
                LABELS examples
                RESOURCE_LOCK faio_echo_example_ports)
        endforeach()
    endif()
endif()
