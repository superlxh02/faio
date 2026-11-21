function(faio_add_experimental_unit target source component)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE ${component} GTest::gtest_main)
    faio_configure_entry(${target} MODE NORMAL)
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 30 LABELS "unit;experimental")
endfunction()

faio_add_experimental_unit(faio_experimental_runtime_tests test_runtime.cpp
        faio::experimental_runtime)

if (FAIO_EXPERIMENTAL_EXECUTION)
    faio_add_experimental_unit(faio_experimental_execution_tests test_execution.cpp
            faio::experimental_execution)
endif ()

if (FAIO_EXPERIMENTAL_REFLECTION)
    faio_add_experimental_unit(faio_experimental_reflection_tests test_reflection.cpp
            faio::experimental_reflection)
    faio_add_experimental_unit(faio_experimental_lifecycle_tests test_lifecycle.cpp
            faio::experimental_lifecycle)
    add_executable(faio_experimental_entry_tests test_entry.cpp)
    faio_configure_entry(faio_experimental_entry_tests MODE ASYNC)
    add_test(NAME faio_experimental_entry_tests COMMAND faio_experimental_entry_tests)
    set_tests_properties(faio_experimental_entry_tests PROPERTIES
            TIMEOUT 30 LABELS "unit;experimental")
endif ()
