cmake_minimum_required(VERSION 3.20)

set(build_command "${CMAKE_COMMAND}" --build "${FAIO_BINARY_DIR}" --parallel 1)
if (NOT FAIO_CONFIG STREQUAL "")
    list(APPEND build_command --config "${FAIO_CONFIG}")
endif ()

foreach (case valid expected_not_exhaustive guard_not_exhaustive scalar_not_exhaustive
        variant_not_exhaustive mismatched_returns invalid_format missing_format_argument)
    execute_process(
            COMMAND ${build_command} --target "faio_match_stdio_${case}"
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE errors)
    if (case STREQUAL "valid")
        if (NOT result STREQUAL "0")
            message(FATAL_ERROR "Valid match/stdio use failed to compile:\n${output}\n${errors}")
        endif ()
    else ()
        if (result STREQUAL "0")
            message(FATAL_ERROR "Invalid match/stdio use compiled successfully: ${case}")
        endif ()
        if (case MATCHES "not_exhaustive$")
            set(expected_diagnostic "faio::match must cover every state")
        elseif (case STREQUAL "mismatched_returns")
            set(expected_diagnostic "faio::match handlers must return exactly the same type")
        else ()
            set(expected_diagnostic "consteval|constant expression|constant-expression|C7595|C2131")
        endif ()
        if (NOT "${output}\n${errors}" MATCHES "${expected_diagnostic}")
            message(FATAL_ERROR "Expected the public contract diagnostic for ${case}:\n${output}\n${errors}")
        endif ()
    endif ()
endforeach ()
