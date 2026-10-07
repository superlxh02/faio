# Every example owns its entry; experimental compile options stay target-local.
foreach(_faio_example IN ITEMS async_main ordinary_main execution_current_thread
    execution_multi_thread execution_tcp_server async_main_execution async_main_execution_current_thread)
    if(_faio_example MATCHES "^execution_|^async_main_execution" OR
       (_faio_example STREQUAL "async_main" AND FAIO_EXPERIMENTAL_EXECUTION))
        if(NOT FAIO_EXPERIMENTAL_EXECUTION)
            continue()
        endif()
        set(_faio_example_component faio::experimental_execution)
    else()
        set(_faio_example_component faio::experimental_runtime)
    endif()
    if(_faio_example MATCHES "^async_main" AND NOT FAIO_EXPERIMENTAL_REFLECTION)
        continue()
    endif()
    add_executable(faio_experimental_${_faio_example} "${_faio_example}.cpp")
    target_link_libraries(faio_experimental_${_faio_example} PRIVATE "${_faio_example_component}")
    set_target_properties(faio_experimental_${_faio_example}
        PROPERTIES CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    if(_faio_example MATCHES "^async_main")
        faio_configure_entry(faio_experimental_${_faio_example} MODE ASYNC)
    else()
        faio_configure_entry(faio_experimental_${_faio_example} MODE NORMAL)
    endif()
    # The TCP server listens indefinitely; only finite examples belong in CTest.
    if(FAIO_BUILD_TESTS AND NOT _faio_example STREQUAL "execution_tcp_server")
        add_test(NAME faio_experimental_${_faio_example}
            COMMAND faio_experimental_${_faio_example})
        set_tests_properties(faio_experimental_${_faio_example}
            PROPERTIES TIMEOUT 30 LABELS experimental-examples)
    endif()
endforeach()
