if (NOT FAIO_EXPERIMENTAL_REFLECTION)
    return()
endif ()

add_custom_target(faio_experimental_examples)

function(faio_add_experimental_example name source)
    set(target "faio_experimental_${name}")
    add_executable(${target} ${source})
    if (ARGN)
        target_link_libraries(${target} PRIVATE ${ARGN})
    endif ()
    faio_configure_entry(${target} MODE ASYNC)
    add_dependencies(faio_experimental_examples ${target})

    if (FAIO_BUILD_TESTS)
        add_test(NAME example_${target} COMMAND ${target})
        set_tests_properties(example_${target} PROPERTIES TIMEOUT 20 LABELS "example;experimental")
    endif ()
endfunction()

faio_add_experimental_example(async_main entry/async_main.cpp)
faio_add_experimental_example(exit_cleanup entry/exit_cleanup.cpp)
faio_add_experimental_example(annotated_task lifecycle/annotated_task.cpp faio::experimental_lifecycle)

if (FAIO_EXPERIMENTAL_EXECUTION)
    faio_add_experimental_example(sender_bridge execution/sender_bridge.cpp faio::experimental_execution)
endif ()
