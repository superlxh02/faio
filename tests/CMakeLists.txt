enable_testing()

find_package(GTest CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_executable(faio_unit_tests
    all_tests.cpp
)

target_link_libraries(faio_unit_tests PRIVATE GTest::gtest GTest::gtest_main faio::faio)

add_test(NAME faio_unit_tests COMMAND faio_unit_tests)

add_executable(faio_default_runtime_tests test_default_runtime_lifecycle.cpp)
target_link_libraries(faio_default_runtime_tests PRIVATE GTest::gtest GTest::gtest_main faio::faio)
add_test(NAME faio_default_runtime_tests COMMAND faio_default_runtime_tests)

add_executable(faio_scheduler_tests test_scheduler.cpp)
target_compile_options(faio_scheduler_tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(faio_scheduler_tests PRIVATE GTest::gtest GTest::gtest_main faio::faio)
add_test(NAME faio_scheduler_tests COMMAND faio_scheduler_tests)
set_tests_properties(faio_scheduler_tests PROPERTIES TIMEOUT 60)
