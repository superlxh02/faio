# -----------------------------------------------------------------------------
# 第三方依赖与测试入口
# -----------------------------------------------------------------------------
# GoogleTest 由根目录的依赖函数提供：可使用已安装包或 Windows 固定版本源码。
# GTest::gtest_main 提供常规测试程序的 main；自行定义 main 的目标只链接 faio。
# Threads 查找保持在此目录，faio::faio 也会传播线程库及自身的平台依赖。
enable_testing()

faio_find_gtest()
find_package(Threads REQUIRED)

# -----------------------------------------------------------------------------
# Windows 测试矩阵：公共模块原始测试 + 独立 Windows I/O 合同
# -----------------------------------------------------------------------------
# 原 POSIX I/O 测试直接使用 socketpair、pipe、mkdtemp 等接口；Windows 分支
# 保留能移植的协程、同步和时间测试，并使用 Windows 网络、文件、IOCP 合同。
# 所有 Windows 目标从 faio::faio 继承系统库、平台宏和编译器参数：
#   MSVC 与 clang-cl（MSVC ABI）：/W4、/utf-8、/permissive-、/Zc:__cplusplus；
#   MinGW GCC：-Wall、-Wextra、-Wpedantic，以及根目录设置的 TLS 编译 launcher。
# 本目录不重复设置这些选项，也不改变不同编译器的测试目标或超时。
if(WIN32)
    add_executable(faio_unit_tests
        test_runtime_task.cpp
        test_sync_primitives.cpp
        test_time_and_net.cpp)
    target_link_libraries(faio_unit_tests PRIVATE GTest::gtest_main faio::faio)
    add_test(NAME faio_unit_tests COMMAND faio_unit_tests)
    set_tests_properties(faio_unit_tests PROPERTIES TIMEOUT 120)

    # 文件存在才建立对应目标；保留可选合同文件及原有目标命名约定。
    # 此组使用 GoogleTest 的 main，各目标的 CTest 超时均为 120 秒。
    foreach(test IN ITEMS
        default_runtime_lifecycle
        worker_io_budget
        windows_contract
        windows_fs_contract
        windows_fs
        windows_iocp)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_${test}.cpp")
            add_executable(faio_${test}_tests test_${test}.cpp)
            target_link_libraries(faio_${test}_tests PRIVATE GTest::gtest_main faio::faio)
            add_test(NAME faio_${test}_tests COMMAND faio_${test}_tests)
            set_tests_properties(faio_${test}_tests PROPERTIES TIMEOUT 120)
        endif()
    endforeach()

    # 网络合同文件自行定义 main，不能再链接 GTest::gtest_main。
    # Winsock、扩展 socket API 和地址解析系统库由 faio::faio 统一传播。
    foreach(test IN ITEMS windows_network)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_${test}.cpp")
            add_executable(faio_${test}_tests test_${test}.cpp)
            target_link_libraries(faio_${test}_tests PRIVATE faio::faio)
            add_test(NAME faio_${test}_tests COMMAND faio_${test}_tests)
            set_tests_properties(faio_${test}_tests PROPERTIES TIMEOUT 120)
        endif()
    endforeach()

    # 三个真实翻译单元共同链接，验证头文件消费和跨 TU 动态 TLS 生命周期。
    # 此程序也有自己的 main；30 秒超时与上述单元测试目标相互独立。
    add_executable(faio_windows_multi_tu_tests
        test_windows_multi_tu.cpp
        windows_multi_tu_other.cpp
        windows_multi_tu_value.cpp)
    target_link_libraries(faio_windows_multi_tu_tests PRIVATE faio::faio)
    add_test(NAME faio_windows_multi_tu_tests COMMAND faio_windows_multi_tu_tests)
    set_tests_properties(faio_windows_multi_tu_tests PROPERTIES TIMEOUT 30)

    # Windows 测试矩阵在此结束，不继续建立后面的原始 POSIX 测试目标。
    return()
endif()

# -----------------------------------------------------------------------------
# Linux/macOS 原始完整矩阵：保留全部目标、源码、宏和测试注册参数
# -----------------------------------------------------------------------------
# 此处所有目标均链接 GTest::gtest_main 与 faio::faio。
# GCC 和采用 GNU 命令行的 Clang 使用原有警告选项；显式设置过选项的目标
# 继续保留各自的 -Wall/-Wextra/-Wpedantic，不借本次整理增删任何编译参数。
# 原始源文件中的平台/能力条件控制具体用例；io_uring 的可用性仍由根目录
# FAIO_ENABLE_IO_URING 和 faio::faio 传播的 FAIO_HAS_IO_URING 决定。

# 公共模块聚合测试及默认运行时生命周期。
add_executable(faio_unit_tests
    all_tests.cpp)
target_link_libraries(faio_unit_tests PRIVATE GTest::gtest_main faio::faio)
add_test(NAME faio_unit_tests COMMAND faio_unit_tests)
set_tests_properties(faio_unit_tests PROPERTIES TIMEOUT 120)

add_executable(faio_default_runtime_tests test_default_runtime_lifecycle.cpp)
target_link_libraries(faio_default_runtime_tests PRIVATE GTest::gtest_main faio::faio)
add_test(NAME faio_default_runtime_tests COMMAND faio_default_runtime_tests)
set_tests_properties(faio_default_runtime_tests PROPERTIES TIMEOUT 60)

# 单线程运行时与调度器；帧分配器测试保留独立的第二个翻译单元。
add_executable(faio_current_thread_tests test_current_thread.cpp)
target_link_libraries(faio_current_thread_tests PRIVATE GTest::gtest_main faio::faio)
add_test(NAME faio_current_thread_tests COMMAND faio_current_thread_tests)
target_compile_options(faio_current_thread_tests PRIVATE -Wall -Wextra -Wpedantic)
set_tests_properties(faio_current_thread_tests PROPERTIES TIMEOUT 60)

add_executable(faio_scheduler_tests
    test_scheduler.cpp
    frame_allocator_test_other_tu.cpp)
target_compile_options(faio_scheduler_tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(faio_scheduler_tests PRIVATE GTest::gtest_main faio::faio)
add_test(NAME faio_scheduler_tests COMMAND faio_scheduler_tests)
set_tests_properties(faio_scheduler_tests PROPERTIES TIMEOUT 60)

# 后端协议不必包含 faio.hpp，可独立定位 reactor 层错误。
add_executable(faio_reactor_tests test_reactor_contract.cpp)
target_link_libraries(faio_reactor_tests PRIVATE GTest::gtest_main faio::faio)
add_test(NAME faio_reactor_tests COMMAND faio_reactor_tests)
set_tests_properties(faio_reactor_tests PROPERTIES TIMEOUT 30)

# 网络、文件系统及组合 I/O 算法各自形成可独立运行的合同。
add_executable(faio_network_tests test_network_contract.cpp)
target_link_libraries(faio_network_tests PRIVATE GTest::gtest_main faio::faio)
add_test(NAME faio_network_tests COMMAND faio_network_tests)
set_tests_properties(faio_network_tests PROPERTIES TIMEOUT 60)

add_executable(faio_filesystem_tests test_fs_contract.cpp)
target_link_libraries(faio_filesystem_tests PRIVATE GTest::gtest_main faio::faio)
add_test(NAME faio_filesystem_tests COMMAND faio_filesystem_tests)
set_tests_properties(faio_filesystem_tests PROPERTIES TIMEOUT 60)

add_executable(faio_io_algorithm_tests test_io_algorithms.cpp)
target_link_libraries(faio_io_algorithm_tests PRIVATE GTest::gtest_main faio::faio)
add_test(NAME faio_io_algorithm_tests COMMAND faio_io_algorithm_tests)
set_tests_properties(faio_io_algorithm_tests PROPERTIES TIMEOUT 60)

# 操作生命周期、引擎公开合同与 Unix socket 封装。
add_executable(faio_io_lifecycle_tests test_io_lifecycle.cpp)
target_link_libraries(faio_io_lifecycle_tests PRIVATE GTest::gtest_main faio::faio)
add_test(NAME faio_io_lifecycle_tests COMMAND faio_io_lifecycle_tests)
set_tests_properties(faio_io_lifecycle_tests PROPERTIES TIMEOUT 60)

add_executable(faio_engine_tests test_engine_contract.cpp)
target_link_libraries(faio_engine_tests PRIVATE GTest::gtest_main faio::faio)
add_test(NAME faio_engine_tests COMMAND faio_engine_tests)
set_tests_properties(faio_engine_tests PROPERTIES TIMEOUT 60)

add_executable(faio_unix_tests test_unix_contract.cpp)
target_link_libraries(faio_unix_tests PRIVATE GTest::gtest_main faio::faio)
add_test(NAME faio_unix_tests COMMAND faio_unix_tests)
set_tests_properties(faio_unix_tests PROPERTIES TIMEOUT 60)

# 结构化取消独立运行，超时保持为 30 秒。
add_executable(faio_structured_cancellation_tests test_structured_cancellation.cpp)
target_link_libraries(faio_structured_cancellation_tests PRIVATE GTest::gtest_main faio::faio)
add_test(NAME faio_structured_cancellation_tests COMMAND faio_structured_cancellation_tests)
set_tests_properties(faio_structured_cancellation_tests PROPERTIES TIMEOUT 30)

# 默认/显式后端选择与原生完成路径分组，便于同一二进制的双后端矩阵。
add_executable(faio_backend_selection_tests test_backend_selection.cpp)
target_link_libraries(faio_backend_selection_tests PRIVATE GTest::gtest_main faio::faio)
add_test(NAME faio_backend_selection_tests COMMAND faio_backend_selection_tests)
set_tests_properties(faio_backend_selection_tests PROPERTIES TIMEOUT 30)

add_executable(faio_native_completion_tests test_native_completion_contract.cpp)
target_link_libraries(faio_native_completion_tests PRIVATE GTest::gtest_main faio::faio)
add_test(NAME faio_native_completion_tests COMMAND faio_native_completion_tests)
set_tests_properties(faio_native_completion_tests PROPERTIES TIMEOUT 30)

# 原生控制并发/背压 seam 只在这个单 TU 目标中打开；其他目标保持常规头配置。
# faio::faio 是 INTERFACE 库，本目标不链接其他包含 uring_backend 的工程对象，
# 因而测试宏不会造成同一程序不同 TU 的定义差异。
add_executable(faio_uring_control_tests test_uring_control_contract.cpp)
target_compile_definitions(faio_uring_control_tests PRIVATE FAIO_URING_CONTROL_TESTS=1)
target_compile_options(faio_uring_control_tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(faio_uring_control_tests PRIVATE GTest::gtest_main faio::faio)
add_test(NAME faio_uring_control_tests COMMAND faio_uring_control_tests)
set_tests_properties(faio_uring_control_tests PROPERTIES TIMEOUT 30)

# scalar 请求的真实 syscall、稳定提交与拥有语义独立验证；同时覆盖不同公共
# 时间头的多 TU 消费，四个原始源文件均保持在此目标中。
add_executable(faio_scalar_request_tests
    test_scalar_request_contract.cpp
    test_scalar_time_header_timeout.cpp
    test_scalar_time_header_time.cpp
    test_scalar_time_header_public.cpp)
target_compile_options(faio_scalar_request_tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(faio_scalar_request_tests PRIVATE GTest::gtest_main faio::faio)
add_test(NAME faio_scalar_request_tests COMMAND faio_scalar_request_tests)
set_tests_properties(faio_scalar_request_tests PROPERTIES TIMEOUT 60)

# worker 私有预算协议使用有限 fake-clock，不启动运行时或原生后端。
add_executable(faio_worker_io_budget_tests test_worker_io_budget.cpp)
target_compile_options(faio_worker_io_budget_tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(faio_worker_io_budget_tests PRIVATE GTest::gtest_main faio::faio)
add_test(NAME faio_worker_io_budget_tests COMMAND faio_worker_io_budget_tests)
set_tests_properties(faio_worker_io_budget_tests PROPERTIES TIMEOUT 30)
