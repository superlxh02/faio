enable_testing()

find_package(GTest CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_executable(faio_unit_tests
        all_tests.cpp
)

target_link_libraries(faio_unit_tests PRIVATE GTest::gtest_main faio::faio)

add_test(NAME faio_unit_tests COMMAND faio_unit_tests)
set_tests_properties(faio_unit_tests PROPERTIES TIMEOUT 120)

add_executable(faio_default_runtime_tests test_default_runtime_lifecycle.cpp)
target_link_libraries(faio_default_runtime_tests PRIVATE GTest::gtest_main faio::faio)
add_test(NAME faio_default_runtime_tests COMMAND faio_default_runtime_tests)
set_tests_properties(faio_default_runtime_tests PROPERTIES TIMEOUT 60)

add_executable(faio_current_thread_tests test_current_thread.cpp)
target_link_libraries(faio_current_thread_tests PRIVATE GTest::gtest_main faio::faio)
add_test(NAME faio_current_thread_tests COMMAND faio_current_thread_tests)
target_compile_options(faio_current_thread_tests PRIVATE -Wall -Wextra -Wpedantic)
set_tests_properties(faio_current_thread_tests PROPERTIES TIMEOUT 60)

add_executable(faio_scheduler_tests test_scheduler.cpp frame_allocator_test_other_tu.cpp)
target_compile_options(faio_scheduler_tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(faio_scheduler_tests PRIVATE GTest::gtest_main faio::faio)
add_test(NAME faio_scheduler_tests COMMAND faio_scheduler_tests)
set_tests_properties(faio_scheduler_tests PROPERTIES TIMEOUT 60)

# 后端契约无需 include faio.hpp，因此可以独立定位 reactor 层的错误。
add_executable(faio_reactor_tests test_reactor_contract.cpp)
target_link_libraries(faio_reactor_tests PRIVATE GTest::gtest_main faio::faio)
add_test(NAME faio_reactor_tests COMMAND faio_reactor_tests)
set_tests_properties(faio_reactor_tests PROPERTIES TIMEOUT 30)

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

add_executable(faio_structured_cancellation_tests test_structured_cancellation.cpp)
target_link_libraries(faio_structured_cancellation_tests PRIVATE GTest::gtest_main faio::faio)
add_test(NAME faio_structured_cancellation_tests COMMAND faio_structured_cancellation_tests)
set_tests_properties(faio_structured_cancellation_tests PROPERTIES TIMEOUT 30)

# 默认/显式后端选择及原生完成路径单独分组，便于同一 binary 的双后端矩阵。
add_executable(faio_backend_selection_tests test_backend_selection.cpp)
target_link_libraries(faio_backend_selection_tests PRIVATE GTest::gtest_main faio::faio)
add_test(NAME faio_backend_selection_tests COMMAND faio_backend_selection_tests)
set_tests_properties(faio_backend_selection_tests PROPERTIES TIMEOUT 30)

add_executable(faio_native_completion_tests test_native_completion_contract.cpp)
target_link_libraries(faio_native_completion_tests PRIVATE GTest::gtest_main faio::faio)
add_test(NAME faio_native_completion_tests COMMAND faio_native_completion_tests)
set_tests_properties(faio_native_completion_tests PROPERTIES TIMEOUT 30)

# 原生控制并发/背压 seam 只存在于这个独立单 TU 可执行文件；其余目标保持正常头文件配置。
# faio::faio 为 INTERFACE 库，目标不链接其他包含 uring_backend 的工程对象，避免跨 TU ODR 差异。
add_executable(faio_uring_control_tests test_uring_control_contract.cpp)
target_compile_definitions(faio_uring_control_tests PRIVATE FAIO_URING_CONTROL_TESTS=1)
target_compile_options(faio_uring_control_tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(faio_uring_control_tests PRIVATE GTest::gtest_main faio::faio)
add_test(NAME faio_uring_control_tests COMMAND faio_uring_control_tests)
set_tests_properties(faio_uring_control_tests PROPERTIES TIMEOUT 30)

# scalar 请求的真实 syscall、稳定提交和拥有语义；与通用桥原合同独立运行。
add_executable(faio_scalar_request_tests test_scalar_request_contract.cpp
        test_scalar_time_header_timeout.cpp test_scalar_time_header_time.cpp
        test_scalar_time_header_public.cpp)
target_compile_options(faio_scalar_request_tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(faio_scalar_request_tests PRIVATE GTest::gtest_main faio::faio)
add_test(NAME faio_scalar_request_tests COMMAND faio_scalar_request_tests)
set_tests_properties(faio_scalar_request_tests PROPERTIES TIMEOUT 60)

# 只测试 worker 私有预算协议，有限 fake-clock 不启动 runtime 或原生后端。
add_executable(faio_worker_io_budget_tests test_worker_io_budget.cpp)
target_compile_options(faio_worker_io_budget_tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(faio_worker_io_budget_tests PRIVATE GTest::gtest_main faio::faio)
add_test(NAME faio_worker_io_budget_tests COMMAND faio_worker_io_budget_tests)
set_tests_properties(faio_worker_io_budget_tests PROPERTIES TIMEOUT 30)
