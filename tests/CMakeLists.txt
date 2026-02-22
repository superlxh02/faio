enable_testing()

find_package(GTest CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_executable(faio_unit_tests
    all_tests.cpp
)

target_include_directories(faio_unit_tests PRIVATE ../include ../thirdparty)
target_link_libraries(faio_unit_tests PRIVATE GTest::gtest GTest::gtest_main Threads::Threads ${LIBS})

add_test(NAME faio_unit_tests COMMAND faio_unit_tests)
