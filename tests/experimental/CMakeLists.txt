# Standalone P0 evidence and the root project's complete experimental suite.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 4.1)
    project(faio_experimental_p0 LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 26)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    set(FAIO_SANITIZERS "" CACHE STRING "P0 sanitizers: address,undefined or thread")
    set(FAIO_EXPERIMENTAL_REFLECTION ON)
    set(FAIO_EXPERIMENTAL_EXECUTION ON)
    get_filename_component(FAIO_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
    include("${FAIO_SOURCE_ROOT}/cmake/FaioExperimentalToolchain.cmake")
    faio_check_experimental_toolchain(TRUE)
    include("${FAIO_SOURCE_ROOT}/cmake/FaioExperimentalDependencies.cmake")
    faio_find_stdexec()
    faio_check_experimental_execution_contract("${FAIO_STDEXEC_SOURCE_DIR}/include")
    enable_testing()
    set(_faio_p0_standalone TRUE)
else()
    set(FAIO_SOURCE_ROOT "${PROJECT_SOURCE_DIR}")
    set(_faio_p0_standalone FALSE)
endif()

option(FAIO_P0_ORIGINAL_STOP_TOKEN_EVIDENCE
    "Register the original unsupported raw std::stop_token compatibility failures" OFF)
set(_faio_p0_probes)
if(FAIO_EXPERIMENTAL_REFLECTION)
    list(APPEND _faio_p0_probes toolchain_probe)
endif()
if(FAIO_EXPERIMENTAL_EXECUTION)
    list(APPEND _faio_p0_probes toolchain_execution_probe toolchain_task_std_stop_token_wrapper_probe)
endif()
foreach(_faio_probe IN LISTS _faio_p0_probes)
    add_executable(faio_${_faio_probe} "${_faio_probe}.cpp")
    target_compile_features(faio_${_faio_probe} PRIVATE cxx_std_26)
    target_compile_options(faio_${_faio_probe} PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    target_include_directories(faio_${_faio_probe} PRIVATE "${FAIO_SOURCE_ROOT}/include")
    set_target_properties(faio_${_faio_probe} PROPERTIES
        CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF CXX_SCAN_FOR_MODULES OFF)
    if(_faio_probe STREQUAL "toolchain_probe")
        target_compile_options(faio_${_faio_probe} PRIVATE -freflection)
    else()
        target_link_libraries(faio_${_faio_probe} PRIVATE faio_stdexec)
    endif()
    if(FAIO_SANITIZERS)
        target_compile_options(faio_${_faio_probe} PRIVATE
            "-fsanitize=${FAIO_SANITIZERS}" -fno-omit-frame-pointer)
        target_link_options(faio_${_faio_probe} PRIVATE "-fsanitize=${FAIO_SANITIZERS}")
    endif()
    add_test(NAME faio_${_faio_probe} COMMAND faio_${_faio_probe})
    set_tests_properties(faio_${_faio_probe} PROPERTIES TIMEOUT 30 LABELS experimental-p0)
endforeach()

# Original failures remain opt-in evidence, not expected-error passing tests.
if(FAIO_EXPERIMENTAL_EXECUTION)
    foreach(_faio_shape IN ITEMS direct starts_on)
        set(_faio_target "faio_toolchain_task_std_stop_token_${_faio_shape}")
        add_executable(${_faio_target} EXCLUDE_FROM_ALL
            "compile_fail/toolchain_task_std_stop_token_${_faio_shape}.cpp")
        target_compile_features(${_faio_target} PRIVATE cxx_std_26)
        set_property(TARGET ${_faio_target} PROPERTY CXX_SCAN_FOR_MODULES OFF)
        target_link_libraries(${_faio_target} PRIVATE faio_stdexec)
        target_compile_options(${_faio_target} PRIVATE -Wall -Wextra -Wpedantic)
        if(FAIO_P0_ORIGINAL_STOP_TOKEN_EVIDENCE)
            add_test(NAME ${_faio_target}
                COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target ${_faio_target})
            set_tests_properties(${_faio_target} PROPERTIES
                TIMEOUT 60 LABELS experimental-original-contract RESOURCE_LOCK faio_experimental_build)
        endif()
    endforeach()
endif()
if(_faio_p0_standalone)
    return()
endif()

if(FAIO_EXPERIMENTAL_REFLECTION)
    set(_faio_entry_contract_source "entry_source_contract/annotated entry.cpp")
    add_executable(faio_experimental_test_async_main_source_contract
        "${_faio_entry_contract_source}" entry_source_contract/helper.cpp)
    set_source_files_properties("${_faio_entry_contract_source}" PROPERTIES
        COMPILE_DEFINITIONS FAIO_ENTRY_SOURCE_OPTION=42
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/entry_source_contract/include")
    target_compile_options(faio_experimental_test_async_main_source_contract PRIVATE -UNDEBUG)
    set_property(TARGET faio_experimental_test_async_main_source_contract PROPERTY UNITY_BUILD ON)
    faio_configure_entry(faio_experimental_test_async_main_source_contract MODE ASYNC
        SOURCE "${_faio_entry_contract_source}")
    add_test(NAME faio_experimental_test_async_main_source_contract
        COMMAND faio_experimental_test_async_main_source_contract)
    set_tests_properties(faio_experimental_test_async_main_source_contract PROPERTIES
        TIMEOUT 30 LABELS experimental-entry)
endif()

file(GLOB _faio_experimental_test_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")
foreach(_faio_source IN LISTS _faio_experimental_test_sources)
    get_filename_component(_faio_name "${_faio_source}" NAME_WE)
    if(_faio_name MATCHES "^test_async_main" AND NOT FAIO_EXPERIMENTAL_REFLECTION)
        continue()
    endif()
    if(_faio_name MATCHES "^test_(execution|task_sender)" OR _faio_name STREQUAL "test_async_main_sender_cancel")
        if(NOT FAIO_EXPERIMENTAL_EXECUTION)
            continue()
        endif()
        set(_faio_component faio::experimental_execution)
    elseif(_faio_name MATCHES "^test_(reflection|async_main)")
        if(NOT FAIO_EXPERIMENTAL_REFLECTION)
            continue()
        endif()
        set(_faio_component faio::experimental_reflection)
    else()
        set(_faio_component faio::experimental_runtime)
    endif()
    set(_faio_target "faio_experimental_${_faio_name}")
    add_executable(${_faio_target} "${_faio_source}")
    target_link_libraries(${_faio_target} PRIVATE "${_faio_component}")
    target_compile_options(${_faio_target} PRIVATE -UNDEBUG)
    set_target_properties(${_faio_target} PROPERTIES
        CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF CXX_SCAN_FOR_MODULES OFF)
    if(_faio_name MATCHES "^test_async_main" AND NOT _faio_name STREQUAL "test_async_main_normal")
        faio_configure_entry(${_faio_target} MODE ASYNC)
    else()
        faio_configure_entry(${_faio_target} MODE NORMAL)
    endif()
    set(_faio_process_exit "")
    set(_faio_process_output "")
    if(_faio_name STREQUAL "test_async_main_exception")
        set(_faio_process_exit 1)
        set(_faio_process_output "entry test failure")
    elseif(_faio_name STREQUAL "test_async_main_annotation_exception")
        set(_faio_process_exit 1)
        set(_faio_process_output "annotated entry failure")
    elseif(_faio_name STREQUAL "test_async_main_unknown_exception")
        set(_faio_process_exit 1)
        set(_faio_process_output "failed with unknown exception")
    elseif(_faio_name STREQUAL "test_async_main_prestarted")
        set(_faio_process_exit 1)
        set(_faio_process_output "运行时已启动或已关闭")
    elseif(_faio_name STREQUAL "test_async_main_exit_code" OR
           _faio_name STREQUAL "test_async_main_options_first")
        set(_faio_process_exit 37)
    elseif(_faio_name STREQUAL "test_async_main_named_overload")
        set(_faio_process_exit 41)
    endif()
    if(NOT _faio_process_exit STREQUAL "")
        add_test(NAME ${_faio_target} COMMAND "${CMAKE_COMMAND}"
            "-DFAIO_PROCESS=$<TARGET_FILE:${_faio_target}>" "-DFAIO_EXPECT_EXIT=${_faio_process_exit}"
            "-DFAIO_EXPECT_OUTPUT=${_faio_process_output}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_process_exit.cmake")
    else()
        add_test(NAME ${_faio_target} COMMAND ${_faio_target})
    endif()
    set_tests_properties(${_faio_target} PROPERTIES TIMEOUT 60 LABELS experimental)
endforeach()

file(GLOB _faio_negative_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/test_*.cpp")
foreach(_faio_source IN LISTS _faio_negative_sources)
    get_filename_component(_faio_name "${_faio_source}" NAME_WE)
    if(_faio_name MATCHES "^test_(reflection|async_main)")
        if(NOT FAIO_EXPERIMENTAL_REFLECTION)
            continue()
        endif()
        set(_faio_component faio::experimental_reflection)
    elseif(_faio_name MATCHES "^test_(execution|task_sender)")
        if(NOT FAIO_EXPERIMENTAL_EXECUTION)
            continue()
        endif()
        set(_faio_component faio::experimental_execution)
    else()
        set(_faio_component faio::experimental_runtime)
    endif()
    set(_faio_target "faio_compile_fail_${_faio_name}")
    add_executable(${_faio_target} EXCLUDE_FROM_ALL "${_faio_source}")
    if(_faio_name STREQUAL "test_async_main_duplicate_entry")
        target_sources(${_faio_target} PRIVATE compile_fail/duplicate_async_entry_other.cpp)
    endif()
    target_link_libraries(${_faio_target} PRIVATE "${_faio_component}")
    set_target_properties(${_faio_target} PROPERTIES
        CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF CXX_SCAN_FOR_MODULES OFF)
    if(_faio_name MATCHES "^test_async_main")
        faio_configure_entry(${_faio_target} MODE ASYNC SOURCE "${_faio_source}")
    endif()
    set(_faio_diagnostic "")
    if(_faio_name STREQUAL "test_reflection_duplicate")
        set(_faio_diagnostic "duplicate runtime_options")
    elseif(_faio_name STREQUAL "test_reflection_invalid_interval")
        set(_faio_diagnostic "invalid io_interval")
    elseif(_faio_name STREQUAL "test_reflection_invalid_workers")
        set(_faio_diagnostic "invalid workers")
    elseif(_faio_name STREQUAL "test_reflection_time_overflow")
        set(_faio_diagnostic "invalid max_io_delay_ns")
    elseif(_faio_name STREQUAL "test_async_main_missing_marker")
        set(_faio_diagnostic "exactly one faio::experimental::main annotation")
    elseif(_faio_name STREQUAL "test_async_main_missing_options")
        set(_faio_diagnostic "requires runtime_options")
    elseif(_faio_name STREQUAL "test_async_main_wrong_signature")
        set(_faio_diagnostic "signature must be")
    elseif(_faio_name STREQUAL "test_async_main_entry_signature")
        set(_faio_diagnostic "ambiguating new declaration")
    elseif(_faio_name STREQUAL "test_async_main_duplicate_entry")
        set(_faio_diagnostic "multiple definition of .*async_main")
    elseif(_faio_name STREQUAL "test_async_main_duplicate_marker")
        set(_faio_diagnostic "exactly one faio::experimental::main annotation")
    elseif(_faio_name STREQUAL "test_async_main_duplicate_options")
        set(_faio_diagnostic "duplicate runtime_options")
    elseif(_faio_name STREQUAL "test_async_main_invalid_workers")
        set(_faio_diagnostic "invalid workers")
    elseif(_faio_name STREQUAL "test_async_main_ordinary_main_conflict")
        set(_faio_diagnostic "redefinition of .*main")
    elseif(_faio_name STREQUAL "test_async_main_macro_collision")
        set(_faio_diagnostic "async_main.h requires main not to be a macro")
    elseif(_faio_name MATCHES "^test_(async_main|reflection)_invalid_backend$")
        set(_faio_diagnostic "invalid io_backend")
    elseif(_faio_name MATCHES "^test_(async_main|reflection)_keep_alive_overflow$")
        set(_faio_diagnostic "invalid blocking_keep_alive_ms")
    elseif(_faio_name STREQUAL "test_async_main_namespace")
        set(_faio_diagnostic "async_main.*has not been declared")
    elseif(_faio_name STREQUAL "test_async_main_name_mismatch")
        set(_faio_diagnostic "requires runtime_options")
    elseif(_faio_name STREQUAL "test_async_main_unnamed_marker")
        set(_faio_diagnostic "undefined reference to .*main")
    elseif(_faio_name STREQUAL "test_execution_multiple_values")
        set(_faio_diagnostic "requires one value tuple")
    endif()
    add_test(NAME ${_faio_target} COMMAND "${CMAKE_COMMAND}"
        "-DFAIO_FAILURE_TARGET=${_faio_target}" "-DFAIO_BUILD_DIR=${CMAKE_BINARY_DIR}"
        "-DFAIO_FAILURE_PATTERN=${_faio_diagnostic}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_compile_failure.cmake")
    set_tests_properties(${_faio_target} PROPERTIES
        TIMEOUT 60 LABELS experimental-compile-fail RESOURCE_LOCK faio_experimental_build)
endforeach()

if(FAIO_EXPERIMENTAL_EXECUTION)
    set(FAIO_EXPERIMENTAL_STRESS_REPETITIONS 100 CACHE STRING
        "Number of repetitions for the separate experimental stress target")
    if(NOT FAIO_EXPERIMENTAL_STRESS_REPETITIONS MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "FAIO_EXPERIMENTAL_STRESS_REPETITIONS must be a positive integer")
    endif()
    add_custom_target(faio_experimental_stress
        COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${CMAKE_BINARY_DIR}" --output-on-failure
            --repeat "until-fail:${FAIO_EXPERIMENTAL_STRESS_REPETITIONS}"
            -R "^faio_experimental_test_(execution_scheduler|execution_spawn|execution_shutdown|task_sender)$"
        DEPENDS faio_experimental_test_execution_scheduler faio_experimental_test_execution_spawn
            faio_experimental_test_execution_shutdown faio_experimental_test_task_sender
        USES_TERMINAL)
endif()

# Core types and singleton definitions must stay identical across language modes.
add_library(faio_experimental_mixed_cxx23 OBJECT mixed_cxx23.cpp)
target_link_libraries(faio_experimental_mixed_cxx23 PRIVATE faio::faio)
target_compile_options(faio_experimental_mixed_cxx23 PRIVATE -UNDEBUG)
set_target_properties(faio_experimental_mixed_cxx23 PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
add_executable(faio_experimental_mixed_standard mixed_cxx26.cpp
    $<TARGET_OBJECTS:faio_experimental_mixed_cxx23>)
target_link_libraries(faio_experimental_mixed_standard PRIVATE faio::experimental_runtime)
if(FAIO_EXPERIMENTAL_EXECUTION)
    target_link_libraries(faio_experimental_mixed_standard PRIVATE faio::experimental_execution)
    target_compile_definitions(faio_experimental_mixed_standard PRIVATE FAIO_MIXED_USE_EXECUTION=1)
endif()
target_compile_options(faio_experimental_mixed_standard PRIVATE -UNDEBUG)
faio_configure_entry(faio_experimental_mixed_standard MODE NORMAL)
add_test(NAME faio_experimental_mixed_standard COMMAND faio_experimental_mixed_standard)
set_tests_properties(faio_experimental_mixed_standard PROPERTIES TIMEOUT 30 LABELS experimental)

add_test(NAME faio_experimental_cmake_contracts COMMAND "${CMAKE_COMMAND}"
    "-DFAIO_SOURCE_ROOT=${FAIO_SOURCE_ROOT}" "-DFAIO_BUILD_DIR=${CMAKE_BINARY_DIR}"
    "-DFAIO_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
    "-DFAIO_STDEXEC_SOURCE_DIR=${FAIO_STDEXEC_SOURCE_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake_contracts.cmake")
set_tests_properties(faio_experimental_cmake_contracts PROPERTIES
    TIMEOUT 120 LABELS experimental-cmake RESOURCE_LOCK faio_experimental_build)

add_test(NAME faio_experimental_entry_cmake_contracts COMMAND "${CMAKE_COMMAND}"
    "-DFAIO_SOURCE_ROOT=${FAIO_SOURCE_ROOT}" "-DFAIO_BUILD_DIR=${CMAKE_BINARY_DIR}"
    "-DFAIO_CXX_COMPILER=${CMAKE_CXX_COMPILER}" -DFAIO_ENTRY_CONTRACTS_ONLY=ON
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake_contracts.cmake")
set_tests_properties(faio_experimental_entry_cmake_contracts PROPERTIES
    TIMEOUT 120 LABELS experimental-entry RESOURCE_LOCK faio_experimental_build)

if(FAIO_INSTALL)
    if(FAIO_EXPERIMENTAL_REFLECTION)
        add_test(NAME faio_experimental_entry_installed_consumer COMMAND "${CMAKE_COMMAND}"
            "-DFAIO_SOURCE_ROOT=${FAIO_SOURCE_ROOT}" "-DFAIO_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DFAIO_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/entry_installed_consumer.cmake")
        set_tests_properties(faio_experimental_entry_installed_consumer PROPERTIES
            TIMEOUT 120 LABELS experimental-entry RESOURCE_LOCK faio_experimental_build)
    endif()
    add_test(NAME faio_experimental_installed_consumer COMMAND "${CMAKE_COMMAND}"
        "-DFAIO_SOURCE_ROOT=${FAIO_SOURCE_ROOT}" "-DFAIO_BUILD_DIR=${CMAKE_BINARY_DIR}"
        "-DFAIO_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DFAIO_INSTALLED_REFLECTION=${FAIO_EXPERIMENTAL_REFLECTION}"
        "-DFAIO_INSTALLED_EXECUTION=${FAIO_EXPERIMENTAL_EXECUTION}"
        "-DFAIO_STDEXEC_SOURCE_DIR=${FAIO_STDEXEC_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/installed_consumer.cmake")
    set_tests_properties(faio_experimental_installed_consumer PROPERTIES
        TIMEOUT 120 LABELS experimental-install RESOURCE_LOCK faio_experimental_build)
endif()
