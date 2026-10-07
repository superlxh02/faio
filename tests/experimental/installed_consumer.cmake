set(_faio_evidence "${FAIO_BUILD_DIR}/experimental-evidence/installed-consumer")
set(_faio_prefix "${_faio_evidence}/prefix")
set(_faio_consumer "${_faio_evidence}/source")
file(MAKE_DIRECTORY "${_faio_evidence}")

function(faio_installed_step name)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE _faio_result
        OUTPUT_VARIABLE _faio_output ERROR_VARIABLE _faio_error)
    file(WRITE "${_faio_evidence}/${name}.log" "${_faio_output}\n${_faio_error}")
    if(NOT _faio_result EQUAL 0)
        message(FATAL_ERROR "Installed consumer ${name} failed: ${_faio_output}\n${_faio_error}")
    endif()
endfunction()

faio_installed_step(install "${CMAKE_COMMAND}" --install "${FAIO_BUILD_DIR}" --prefix "${_faio_prefix}")
file(COPY "${FAIO_SOURCE_ROOT}/tests/experimental/installed_consumer/" DESTINATION "${_faio_consumer}")

# Configuration of the copied project needs only this installation and its base
# system dependencies. Neither the checkout nor its external Git repository is
# passed into the consumer's configure command.
file(GLOB_RECURSE _faio_exports "${_faio_prefix}/*faio*Targets.cmake" "${_faio_prefix}/*faio*Config.cmake")
foreach(_faio_export IN LISTS _faio_exports)
    file(READ "${_faio_export}" _faio_export_text)
    foreach(_faio_forbidden IN ITEMS "${FAIO_SOURCE_ROOT}" "${FAIO_STDEXEC_SOURCE_DIR}")
        if(_faio_forbidden)
            string(FIND "${_faio_export_text}" "${_faio_forbidden}" _faio_found)
            if(NOT _faio_found EQUAL -1)
                message(FATAL_ERROR "Installed export contains checkout path: ${_faio_export}")
            endif()
        endif()
    endforeach()
endforeach()
if(FAIO_INSTALLED_EXECUTION)
    file(GLOB_RECURSE _faio_vendored_stdexec "${_faio_prefix}/*/faio-third-party/stdexec/stdexec/execution.hpp")
    file(GLOB_RECURSE _faio_vendored_task "${_faio_prefix}/*/faio-third-party/stdexec/exec/task.hpp")
    file(GLOB_RECURSE _faio_license "${_faio_prefix}/*/licenses/faio/stdexec-LICENSE.txt")
    file(GLOB_RECURSE _faio_identity "${_faio_prefix}/*/licenses/faio/experimental_dependencies.lock.json")
    if(NOT _faio_vendored_stdexec OR NOT _faio_vendored_task OR NOT _faio_license OR NOT _faio_identity)
        message(FATAL_ERROR "Vendored stdexec headers, license, or commit identity is missing")
    endif()
endif()

faio_installed_step(configure "${CMAKE_COMMAND}" -S "${_faio_consumer}" -B "${_faio_evidence}/build"
    -G Ninja "-DCMAKE_CXX_COMPILER=${FAIO_CXX_COMPILER}" "-DCMAKE_PREFIX_PATH=${_faio_prefix}"
    "-DFAIO_CONSUMER_REFLECTION=${FAIO_INSTALLED_REFLECTION}"
    "-DFAIO_CONSUMER_EXECUTION=${FAIO_INSTALLED_EXECUTION}"
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF)
faio_installed_step(build "${CMAKE_COMMAND}" --build "${_faio_evidence}/build" --parallel 1)
get_filename_component(_faio_cmake_bin "${CMAKE_COMMAND}" DIRECTORY)
find_program(_faio_ctest NAMES ctest HINTS "${_faio_cmake_bin}" REQUIRED)
faio_installed_step(run "${_faio_ctest}" --test-dir "${_faio_evidence}/build" --output-on-failure)
set(_faio_rejected_components unrecognized_component)
if(NOT FAIO_INSTALLED_REFLECTION)
    list(APPEND _faio_rejected_components experimental_reflection)
endif()
if(NOT FAIO_INSTALLED_EXECUTION)
    list(APPEND _faio_rejected_components experimental_execution)
endif()
foreach(_faio_component IN LISTS _faio_rejected_components)
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${_faio_consumer}"
        -B "${_faio_evidence}/reject-${_faio_component}" -G Ninja
        "-DCMAKE_CXX_COMPILER=${FAIO_CXX_COMPILER}" "-DCMAKE_PREFIX_PATH=${_faio_prefix}"
        "-DFAIO_CONSUMER_REJECT_COMPONENT=${_faio_component}"
        RESULT_VARIABLE _faio_result OUTPUT_VARIABLE _faio_output ERROR_VARIABLE _faio_error)
    file(WRITE "${_faio_evidence}/reject-${_faio_component}.log" "${_faio_output}\n${_faio_error}")
    if(_faio_result EQUAL 0 OR "${_faio_output}\n${_faio_error}" MATCHES "Unexpectedly accepted"
       OR NOT _faio_error MATCHES "faio_FOUND to FALSE")
        message(FATAL_ERROR "Installed package accepted missing/unknown ${_faio_component}")
    endif()
endforeach()
message(STATUS "Installed consumer passed using ${_faio_prefix}")
