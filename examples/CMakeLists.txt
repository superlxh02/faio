add_custom_target(faio_examples)

function(faio_add_example name source component)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ${component})
    add_dependencies(faio_examples ${name})

    if (FAIO_BUILD_TESTS)
        add_test(NAME example_${name} COMMAND ${name} ${ARGN})
        set_tests_properties(example_${name} PROPERTIES TIMEOUT 20 LABELS example)
    endif ()
endfunction()

faio_add_example(quickstart quickstart.cpp faio::faio)
faio_add_example(match_basics common/match.cpp faio::faio)
faio_add_example(stdio_console stdio/console.cpp faio::faio)
faio_add_example(coroutine_task coroutine/task.cpp faio::faio)
faio_add_example(composition_basics coroutine/composition_basics.cpp faio::faio)
faio_add_example(structured_concurrency coroutine/structured_concurrency.cpp faio::faio)
faio_add_example(batch_results coroutine/batch_results.cpp faio::faio)
faio_add_example(race_strategies coroutine/race_strategies.cpp faio::faio)
faio_add_example(coroutine_lifecycle coroutine/lifecycle.cpp faio::faio)
faio_add_example(cancellation_deadline coroutine/cancellation_deadline.cpp faio::faio)
faio_add_example(coroutine_sequence coroutine/sequence.cpp faio::faio)
faio_add_example(custom_awaiter coroutine/custom_awaiter.cpp faio::faio)
faio_add_example(serial_domain coroutine/serial_domain.cpp faio::faio)
faio_add_example(blocking_work runtime/execution.cpp faio::faio)
faio_add_example(sync_shared_state sync/shared_state.cpp faio::faio)
faio_add_example(sync_coordination sync/coordination.cpp faio::faio)
faio_add_example(io_stream io/stream.cpp faio::faio)
faio_add_example(file_and_directory fs/file_and_directory.cpp faio::faio)
faio_add_example(tcp_echo net/tcp_echo.cpp faio::faio)
faio_add_example(udp_echo net/udp_echo.cpp faio::faio)

if (NOT WIN32)
    faio_add_example(unix_echo net/unix_echo.cpp faio::faio)
endif ()

if (FAIO_ENABLE_TLS)
    set(faio_example_tls_fixtures "${PROJECT_SOURCE_DIR}/tests/fixtures")
    faio_add_example(tls_echo tls/echo.cpp faio::tls
            "${faio_example_tls_fixtures}/server.pem"
            "${faio_example_tls_fixtures}/server.key"
            "${faio_example_tls_fixtures}/ca.pem")
endif ()

if (FAIO_ENABLE_PROCESS)
    faio_add_example(process_command process/command.cpp faio::process)
endif ()
