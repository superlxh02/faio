set(_faio_evidence "${FAIO_BUILD_DIR}/experimental-evidence/cmake-contracts")
file(MAKE_DIRECTORY "${_faio_evidence}")

function(faio_configure_contract name source expect_success diagnostic)
    set(_faio_source "${_faio_evidence}/${name}/source")
    set(_faio_binary "${_faio_evidence}/${name}/build")
    file(MAKE_DIRECTORY "${_faio_source}")
    file(WRITE "${_faio_source}/CMakeLists.txt" "${source}")
    file(WRITE "${_faio_source}/main.cpp" "int main() {}\n")
    file(WRITE "${_faio_source}/helper.cpp" "void helper() {}\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${_faio_source}" -B "${_faio_binary}"
        -G Ninja "-DCMAKE_CXX_COMPILER=${FAIO_CXX_COMPILER}"
        RESULT_VARIABLE _faio_result OUTPUT_VARIABLE _faio_output ERROR_VARIABLE _faio_error)
    set(_faio_log "${_faio_output}\n${_faio_error}")
    file(WRITE "${_faio_evidence}/${name}.log" "${_faio_log}")
    if(expect_success AND NOT _faio_result EQUAL 0)
        message(FATAL_ERROR "${name}: configuration unexpectedly failed: ${_faio_log}")
    elseif(NOT expect_success AND _faio_result EQUAL 0)
        message(FATAL_ERROR "${name}: configuration unexpectedly succeeded")
    endif()
    if(diagnostic AND NOT _faio_log MATCHES "${diagnostic}")
        message(FATAL_ERROR "${name}: missing expected diagnostic '${diagnostic}': ${_faio_log}")
    endif()
endfunction()

set(_faio_preamble "cmake_minimum_required(VERSION 4.1)\nproject(contract LANGUAGES CXX)\n")
if(NOT FAIO_ENTRY_CONTRACTS_ONLY)
set(_faio_module "${FAIO_SOURCE_ROOT}/cmake/FaioExperimental.cmake")
faio_configure_contract(off_missing_stdexec
    "${_faio_preamble}set(FAIO_STDEXEC_SOURCE_DIR /nonexistent/faio-contract/stdexec)\nset(FAIO_ENABLE_EXPERIMENTAL OFF CACHE BOOL \"\")\ninclude(\"${_faio_module}\")\nif(TARGET faio_stdexec OR TARGET faio_experimental_runtime)\nmessage(FATAL_ERROR \"OFF unexpectedly created experimental targets\")\nendif()\n"
    TRUE "")
faio_configure_contract(async_disabled
    "${_faio_preamble}set(FAIO_ENABLE_EXPERIMENTAL OFF CACHE BOOL \"\")\nset(FAIO_EXPERIMENTAL_ENTRY_MODE ASYNC CACHE STRING \"\")\ninclude(\"${_faio_module}\")\n"
    FALSE "ASYNC entry requires")
faio_configure_contract(async_reflection_disabled
    "${_faio_preamble}set(FAIO_ENABLE_EXPERIMENTAL ON CACHE BOOL \"\")\nset(FAIO_EXPERIMENTAL_REFLECTION OFF CACHE BOOL \"\")\nset(FAIO_EXPERIMENTAL_ENTRY_MODE ASYNC CACHE STRING \"\")\ninclude(\"${_faio_module}\")\n"
    FALSE "ASYNC entry requires")
faio_configure_contract(invalid_mode
    "${_faio_preamble}set(FAIO_EXPERIMENTAL_ENTRY_MODE UNKNOWN CACHE STRING \"\")\ninclude(\"${_faio_module}\")\n"
    FALSE "must be NORMAL or ASYNC")
endif()

set(_faio_entry "${FAIO_SOURCE_ROOT}/cmake/FaioExperimentalEntry.cmake")
faio_configure_contract(entry_library
    "${_faio_preamble}include(\"${_faio_entry}\")\nadd_library(app INTERFACE)\nfaio_configure_entry(app MODE NORMAL)\n"
    FALSE "requires an executable target")
faio_configure_contract(entry_conflict
    "${_faio_preamble}include(\"${_faio_entry}\")\nadd_library(reflection INTERFACE)\nadd_library(faio::experimental_reflection ALIAS reflection)\nadd_executable(app main.cpp)\nfaio_configure_entry(app MODE NORMAL)\nfaio_configure_entry(app MODE ASYNC)\n"
    FALSE "cannot set conflicting modes")
set(_faio_entry_preamble "${_faio_preamble}include(\"${_faio_entry}\")\nadd_library(reflection INTERFACE)\nadd_library(faio::experimental_reflection ALIAS reflection)\n")
set(_faio_entry_checks [=[
get_target_property(wrapper app FAIO_ENTRY_WRAPPER)
get_target_property(sources app SOURCES)
if(wrapper OR NOT "main.cpp" IN_LIST sources)
  message(FATAL_ERROR "Entry configuration must keep original sources without a generated wrapper")
endif()
get_target_property(definitions app COMPILE_DEFINITIONS)
if(definitions MATCHES "ASYNC_MAIN_ENABLED")
  message(FATAL_ERROR "The one-shot main macro must not depend on a target-wide switch")
endif()
faio_configure_entry(app MODE ASYNC)
get_target_property(repeated app SOURCES)
if(NOT repeated STREQUAL sources)
  message(FATAL_ERROR "Repeated entry configuration must preserve sources")
endif()
]=])
faio_configure_contract(entry_no_wrapper
    "${_faio_entry_preamble}add_executable(app main.cpp)\nfaio_configure_entry(app MODE ASYNC)\n${_faio_entry_checks}"
    TRUE "")
faio_configure_contract(entry_multiple_sources
    "${_faio_entry_preamble}add_executable(app main.cpp helper.cpp)\nfaio_configure_entry(app MODE ASYNC)\n${_faio_entry_checks}"
    TRUE "")
set(_faio_source_checks [=[
get_source_file_property(definitions main.cpp COMPILE_DEFINITIONS)
if(NOT definitions STREQUAL "ENTRY_SOURCE_SETTING=1")
  message(FATAL_ERROR "Source compile definitions were lost")
endif()
get_source_file_property(header_only main.cpp HEADER_FILE_ONLY)
if(header_only)
  message(FATAL_ERROR "Entry configuration changed the original source")
endif()
]=])
faio_configure_contract(entry_explicit_source
    "${_faio_entry_preamble}add_executable(app main.cpp helper.cpp)\nadd_executable(ordinary main.cpp)\nset_source_files_properties(main.cpp PROPERTIES COMPILE_DEFINITIONS ENTRY_SOURCE_SETTING=1)\nfaio_configure_entry(app MODE ASYNC SOURCE main.cpp)\n${_faio_entry_checks}${_faio_source_checks}"
    TRUE "")
faio_configure_contract(entry_unknown_source
    "${_faio_entry_preamble}add_executable(app main.cpp)\nfaio_configure_entry(app MODE ASYNC SOURCE missing.cpp)\n"
    FALSE "SOURCE must belong to the executable target")
faio_configure_contract(entry_source_conflict
    "${_faio_entry_preamble}add_executable(app main.cpp helper.cpp)\nfaio_configure_entry(app MODE ASYNC SOURCE main.cpp)\nfaio_configure_entry(app MODE ASYNC SOURCE helper.cpp)\n"
    FALSE "cannot set conflicting entry sources")
faio_configure_contract(entry_normal_source
    "${_faio_entry_preamble}add_executable(app main.cpp)\nfaio_configure_entry(app MODE NORMAL SOURCE main.cpp)\n"
    FALSE "SOURCE is only valid with MODE ASYNC")
faio_configure_contract(entry_generated_source
    "${_faio_entry_preamble}add_executable(app main.cpp)\nset_source_files_properties(main.cpp PROPERTIES GENERATED ON)\nfaio_configure_entry(app MODE ASYNC)\n${_faio_entry_checks}"
    TRUE "")
if(FAIO_ENTRY_CONTRACTS_ONLY)
    message(STATUS "faio entry CMake contracts passed")
    return()
endif()

# Configure gates use CMake's actual system/compiler detection in normal builds.
# Here each unsupported identity is deliberately injected after project() so the
# gate's diagnostic is tested without requiring an unrelated cross toolchain.
set(_faio_toolchain "${FAIO_SOURCE_ROOT}/cmake/FaioExperimentalToolchain.cmake")
faio_configure_contract(unsupported_cmake
    "${_faio_preamble}set(CMAKE_VERSION 4.0.0)\ninclude(\"${_faio_toolchain}\")\nfaio_check_experimental_toolchain(FALSE)\n"
    FALSE "CMake >= 4.1")
foreach(_faio_case IN ITEMS gcc16_0 gcc16_2 clang macos windows)
    set(_faio_identity "")
    if(_faio_case STREQUAL gcc16_0)
        set(_faio_identity "set(CMAKE_CXX_COMPILER_VERSION 16.0.1)")
    elseif(_faio_case STREQUAL gcc16_2)
        set(_faio_identity "set(CMAKE_CXX_COMPILER_VERSION 16.2.1)")
    elseif(_faio_case STREQUAL clang)
        set(_faio_identity "set(CMAKE_CXX_COMPILER_ID Clang)")
    elseif(_faio_case STREQUAL macos)
        set(_faio_identity "set(CMAKE_SYSTEM_NAME Darwin)")
    elseif(_faio_case STREQUAL windows)
        set(_faio_identity "set(CMAKE_SYSTEM_NAME Windows)")
    endif()
    faio_configure_contract("unsupported_${_faio_case}"
        "${_faio_preamble}${_faio_identity}\ninclude(\"${_faio_toolchain}\")\nfaio_check_experimental_toolchain(FALSE)\n"
        FALSE "Linux \\+ GCC 16.1.x")
endforeach()

file(WRITE "${_faio_evidence}/direct_include.cpp"
    "#include <faio/detail/experimental/platform.hpp>\nint main() {}\n")
foreach(_faio_case IN ITEMS cxx23 non_linux clang gcc16_0 gcc16_2)
    set(_faio_flags -std=c++26)
    set(_faio_expected "GCC 16.1.x is required")
    if(_faio_case STREQUAL cxx23)
        set(_faio_flags -std=c++23)
        set(_faio_expected "C[+][+]26 is required")
    elseif(_faio_case STREQUAL non_linux)
        list(APPEND _faio_flags -U__linux__)
        set(_faio_expected "Linux is required")
    elseif(_faio_case STREQUAL clang)
        list(APPEND _faio_flags -D__clang__=1)
    elseif(_faio_case STREQUAL gcc16_0)
        list(APPEND _faio_flags -U__GNUC_MINOR__ -D__GNUC_MINOR__=0)
    elseif(_faio_case STREQUAL gcc16_2)
        list(APPEND _faio_flags -U__GNUC_MINOR__ -D__GNUC_MINOR__=2)
    endif()
    execute_process(COMMAND "${FAIO_CXX_COMPILER}" ${_faio_flags}
        "-I${FAIO_SOURCE_ROOT}/include" -c "${_faio_evidence}/direct_include.cpp"
        -o "${_faio_evidence}/direct_include.o"
        RESULT_VARIABLE _faio_result OUTPUT_VARIABLE _faio_output ERROR_VARIABLE _faio_error)
    file(WRITE "${_faio_evidence}/direct_${_faio_case}.log" "${_faio_output}\n${_faio_error}")
    if(_faio_result EQUAL 0 OR NOT _faio_error MATCHES "${_faio_expected}")
        message(FATAL_ERROR "Direct include ${_faio_case} did not reject with '${_faio_expected}': ${_faio_error}")
    endif()
endforeach()
message(STATUS "faio experimental CMake contracts passed")
