cmake_minimum_required(VERSION 3.20)

set(build_command "${CMAKE_COMMAND}" --build "${FAIO_BINARY_DIR}" --parallel 1)
if (NOT FAIO_CONFIG STREQUAL "")
    list(APPEND build_command --config "${FAIO_CONFIG}")
endif ()

foreach (case valid invalid_ipv4 invalid_ipv6 invalid_port embedded_nul)
    execute_process(
            COMMAND ${build_command} --target "faio_socket_addr_literal_${case}"
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE errors)
    if (case STREQUAL "valid")
        if (NOT result STREQUAL "0")
            message(FATAL_ERROR "Valid socket_addr literals failed to compile:\n${output}\n${errors}")
        endif ()
    else ()
        if (result STREQUAL "0")
            message(FATAL_ERROR "Invalid socket_addr literal compiled successfully: ${case}")
        endif ()
        if (NOT "${output}\n${errors}" MATCHES "consteval|constant expression|constant-expression|C7595|C2131")
            message(FATAL_ERROR "Expected a constant-evaluation diagnostic for ${case}:\n${output}\n${errors}")
        endif ()
    endif ()
endforeach ()
