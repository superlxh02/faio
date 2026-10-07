include_guard(GLOBAL)

# The experimental development image supplies defaults through ENV, including
# for IDE-created containers which do not use our presets. Cache values, -D
# options and presets still take precedence; ordinary environments stay OFF.
function(_faio_experimental_env_default output name fallback)
    set(_faio_default "${fallback}")
    if(DEFINED ENV{${name}} AND NOT "$ENV{${name}}" STREQUAL "")
        set(_faio_default "$ENV{${name}}")
    endif()
    set(${output} "${_faio_default}" PARENT_SCOPE)
endfunction()
_faio_experimental_env_default(_faio_enable_default FAIO_ENABLE_EXPERIMENTAL OFF)
_faio_experimental_env_default(_faio_reflection_default FAIO_EXPERIMENTAL_REFLECTION ON)
_faio_experimental_env_default(_faio_execution_default FAIO_EXPERIMENTAL_EXECUTION ON)
_faio_experimental_env_default(_faio_entry_default FAIO_EXPERIMENTAL_ENTRY_MODE NORMAL)
option(FAIO_ENABLE_EXPERIMENTAL "Enable Linux/GCC 16.1 experimental components" ${_faio_enable_default})
option(FAIO_EXPERIMENTAL_REFLECTION "Build the experimental reflection component" ${_faio_reflection_default})
option(FAIO_EXPERIMENTAL_EXECUTION "Build the experimental execution component" ${_faio_execution_default})
set(FAIO_EXPERIMENTAL_ENTRY_MODE "${_faio_entry_default}" CACHE STRING "NORMAL or ASYNC executable entry organization")
unset(_faio_enable_default)
unset(_faio_reflection_default)
unset(_faio_execution_default)
unset(_faio_entry_default)
set_property(CACHE FAIO_EXPERIMENTAL_ENTRY_MODE PROPERTY STRINGS NORMAL ASYNC)
include("${CMAKE_CURRENT_LIST_DIR}/FaioExperimentalEntry.cmake")
if(NOT FAIO_EXPERIMENTAL_ENTRY_MODE MATCHES "^(NORMAL|ASYNC)$")
    message(FATAL_ERROR "FAIO_EXPERIMENTAL_ENTRY_MODE must be NORMAL or ASYNC")
endif()
if(FAIO_EXPERIMENTAL_ENTRY_MODE STREQUAL "ASYNC"
   AND (NOT FAIO_ENABLE_EXPERIMENTAL OR NOT FAIO_EXPERIMENTAL_REFLECTION))
    message(FATAL_ERROR "ASYNC entry requires FAIO_ENABLE_EXPERIMENTAL and FAIO_EXPERIMENTAL_REFLECTION")
endif()
if(NOT FAIO_ENABLE_EXPERIMENTAL)
    return()
endif()

include("${CMAKE_CURRENT_LIST_DIR}/FaioExperimentalToolchain.cmake")
faio_check_experimental_toolchain(${FAIO_EXPERIMENTAL_REFLECTION})

add_library(faio_experimental_runtime INTERFACE)
add_library(faio::experimental_runtime ALIAS faio_experimental_runtime)
set_target_properties(faio_experimental_runtime PROPERTIES EXPORT_NAME experimental_runtime)
target_compile_features(faio_experimental_runtime INTERFACE cxx_std_26)
target_link_libraries(faio_experimental_runtime INTERFACE faio::faio)
set(_faio_experimental_targets faio_experimental_runtime)

if(FAIO_EXPERIMENTAL_REFLECTION)
    add_library(faio_experimental_reflection INTERFACE)
    add_library(faio::experimental_reflection ALIAS faio_experimental_reflection)
    set_target_properties(faio_experimental_reflection PROPERTIES EXPORT_NAME experimental_reflection)
    target_link_libraries(faio_experimental_reflection INTERFACE faio::experimental_runtime)
    target_compile_options(faio_experimental_reflection INTERFACE -freflection)
    target_compile_definitions(faio_experimental_reflection INTERFACE FAIO_EXPERIMENTAL_HAS_REFLECTION=1)
    list(APPEND _faio_experimental_targets faio_experimental_reflection)
endif()

if(FAIO_EXPERIMENTAL_EXECUTION)
    include("${CMAKE_CURRENT_LIST_DIR}/FaioExperimentalDependencies.cmake")
    faio_find_stdexec()
    faio_check_experimental_execution_contract("${FAIO_STDEXEC_SOURCE_DIR}/include")
    set_target_properties(faio_stdexec PROPERTIES EXPORT_NAME experimental_stdexec)
    add_library(faio_experimental_execution INTERFACE)
    add_library(faio::experimental_execution ALIAS faio_experimental_execution)
    set_target_properties(faio_experimental_execution PROPERTIES EXPORT_NAME experimental_execution)
    target_link_libraries(faio_experimental_execution INTERFACE faio::experimental_runtime faio_stdexec)
    target_compile_definitions(faio_experimental_execution INTERFACE FAIO_EXPERIMENTAL_HAS_EXECUTION=1)
    list(APPEND _faio_experimental_targets faio_stdexec faio_experimental_execution)
endif()

if(FAIO_INSTALL)
    install(TARGETS ${_faio_experimental_targets} EXPORT faioExperimentalTargets)
    install(EXPORT faioExperimentalTargets FILE faioExperimentalTargets.cmake
        NAMESPACE faio:: DESTINATION "${CMAKE_INSTALL_DATADIR}/cmake/faio")
    configure_file("${CMAKE_CURRENT_LIST_DIR}/faioExperimentalConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/faioExperimentalConfig.cmake" @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/faioExperimentalConfig.cmake"
        "${CMAKE_CURRENT_LIST_DIR}/FaioExperimentalToolchain.cmake"
        "${CMAKE_CURRENT_LIST_DIR}/FaioExperimentalEntry.cmake"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/cmake/faio")
    if(FAIO_EXPERIMENTAL_EXECUTION)
        install(DIRECTORY "${FAIO_STDEXEC_SOURCE_DIR}/include/stdexec"
            "${FAIO_STDEXEC_SOURCE_DIR}/include/exec"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/faio-third-party/stdexec")
        install(FILES "${FAIO_STDEXEC_SOURCE_DIR}/LICENSE.txt"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/faio" RENAME stdexec-LICENSE.txt)
        install(FILES "${PROJECT_SOURCE_DIR}/scripts/experimental_dependencies.lock.json"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/faio")
    endif()
endif()
