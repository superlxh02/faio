set(_faio_evidence "${FAIO_BUILD_DIR}/experimental-evidence/entry-installed-consumer")
set(_faio_prefix "${_faio_evidence}/install")
set(_faio_consumer "${_faio_evidence}/build")
file(MAKE_DIRECTORY "${_faio_evidence}")
function(faio_entry_command name)
    execute_process(COMMAND ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    file(WRITE "${_faio_evidence}/${name}.log" "${output}\n${error}")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Installed entry ${name} failed: ${output}\n${error}")
    endif()
endfunction()
faio_entry_command(install "${CMAKE_COMMAND}" --install "${FAIO_BUILD_DIR}" --prefix "${_faio_prefix}")
faio_entry_command(configure "${CMAKE_COMMAND}"
    -S "${FAIO_SOURCE_ROOT}/tests/experimental/entry_installed_consumer" -B "${_faio_consumer}"
    -G Ninja "-DCMAKE_CXX_COMPILER=${FAIO_CXX_COMPILER}"
    "-Dfaio_DIR=${_faio_prefix}/share/cmake/faio")
faio_entry_command(build "${CMAKE_COMMAND}" --build "${_faio_consumer}" --target entry -j2)
faio_entry_command(run "${_faio_consumer}/entry")
message(STATUS "Installed main annotation entry passed")
