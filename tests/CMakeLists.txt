add_executable(faio_unit_tests
        test_coroutine.cpp
        test_coroutine_awaiter.cpp
        test_stop_boundary.cpp
        test_runtime.cpp
        test_sync.cpp
        test_io.cpp
        test_shared_task.cpp
        test_early_race.cpp
        test_coroutine_on.cpp
        test_stream.cpp
        test_serial_domain.cpp
        test_async_sequence.cpp
        test_net.cpp)
target_link_libraries(faio_unit_tests PRIVATE faio::faio GTest::gtest_main)
add_test(NAME faio_unit_tests COMMAND faio_unit_tests)
set_tests_properties(faio_unit_tests PROPERTIES TIMEOUT 60 LABELS unit)

add_test(NAME faio_public_header_layout
        COMMAND ${CMAKE_COMMAND} "-DFAIO_HEADERS=${PROJECT_SOURCE_DIR}/include/faio"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_public_header_layout.cmake")
set_tests_properties(faio_public_header_layout PROPERTIES LABELS unit)

add_test(NAME coro_contract_awaiter COMMAND faio_unit_tests --gtest_filter=CoroutineContractAwaiter.*)
set_tests_properties(coro_contract_awaiter PROPERTIES TIMEOUT 60 LABELS unit)

add_test(NAME coro_outcome COMMAND faio_unit_tests --gtest_filter=CoroutineOutcome.*)
add_test(NAME coro_settle COMMAND faio_unit_tests --gtest_filter=CoroutineSettle.*)
add_test(NAME coro_cleanup COMMAND faio_unit_tests --gtest_filter=CoroutineCleanup.*)
add_test(NAME coro_group COMMAND faio_unit_tests --gtest_filter=CoroutineGroup.*)
add_test(NAME coro_settled COMMAND faio_unit_tests --gtest_filter=CoroutineSettled.*:CoroutineSelectSuccess.*)
set_tests_properties(coro_outcome coro_settle coro_cleanup coro_group coro_settled PROPERTIES TIMEOUT 60 LABELS unit)

add_test(NAME coro_stop COMMAND faio_unit_tests --gtest_filter=CoroutineStopBoundary.*)
add_test(NAME coro_deadline COMMAND faio_unit_tests --gtest_filter=CoroutineDeadline.*)
set_tests_properties(coro_stop coro_deadline PROPERTIES TIMEOUT 60 LABELS unit)

if (FAIO_ENABLE_TLS)
    add_executable(faio_tls_tests test_tls.cpp)
    target_link_libraries(faio_tls_tests PRIVATE faio::tls GTest::gtest_main)
    target_compile_definitions(faio_tls_tests PRIVATE
            FAIO_TLS_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
    add_test(NAME faio_tls_tests COMMAND faio_tls_tests)
    set_tests_properties(faio_tls_tests PROPERTIES TIMEOUT 30 LABELS unit)
endif ()

if (FAIO_ENABLE_PROCESS)
    add_executable(faio_process_test_child process_child.cpp)
    target_compile_features(faio_process_test_child PRIVATE cxx_std_23)
    if (WIN32)
        target_link_libraries(faio_process_test_child PRIVATE shell32)
    endif ()
    add_executable(faio_process_tests test_process.cpp)
    target_link_libraries(faio_process_tests PRIVATE faio::process GTest::gtest_main)
    target_compile_definitions(faio_process_tests PRIVATE
            FAIO_PROCESS_TEST_CHILD="$<TARGET_FILE:faio_process_test_child>")
    add_dependencies(faio_process_tests faio_process_test_child)
    add_test(NAME faio_process_tests COMMAND faio_process_tests)
    set_tests_properties(faio_process_tests PROPERTIES TIMEOUT 30 LABELS unit)
endif ()
