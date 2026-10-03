# 每个示例仅由自己的 cpp 构建，不链接其他示例或公共示例实现。
foreach (example IN ITEMS
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
endforeach ()

# 仅注册有限示例。协程与同步各自一个程序，函数内顺序演示不同用法。
if (FAIO_BUILD_TESTS)
    enable_testing()
    add_test(NAME coroutine_basics COMMAND coroutine_basics)
    add_test(NAME coroutine_sync COMMAND coroutine_sync)
    add_test(NAME blocking_thread_pool COMMAND blocking_thread_pool)
    add_test(NAME tcp_counter_server COMMAND tcp_counter_server --self-test)
    # 文件示例在自己的构建子目录中运行，生成的 test/main.c 不写进源码目录。
    set(file_example_workdir "${CMAKE_CURRENT_BINARY_DIR}/file_and_directory_workdir")
    file(MAKE_DIRECTORY "${file_example_workdir}")
    add_test(NAME file_and_directory COMMAND file_and_directory)
    set_tests_properties(file_and_directory PROPERTIES WORKING_DIRECTORY "${file_example_workdir}")
    set_tests_properties(coroutine_basics coroutine_sync blocking_thread_pool tcp_counter_server file_and_directory
            PROPERTIES TIMEOUT 30 LABELS examples)
endif ()
