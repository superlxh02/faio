cmake_minimum_required(VERSION 3.28)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(faioModules LANGUAGES CXX)
    option(FAIO_INSTALL "Install the faio module package" ON)
endif()

if(NOT CMAKE_GENERATOR MATCHES "^Ninja")
    message(FATAL_ERROR "The faio module build requires Ninja (1.11 or newer)")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
   AND NOT EXISTS "${CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS}")
    message(FATAL_ERROR "The faio module build requires clang-scan-deps from the selected Clang distribution")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND NOT CMAKE_CXX_SCANDEP_SOURCE)
    message(FATAL_ERROR "The selected Apple Clang cannot scan modules with this CMake version. Set CMAKE_CXX_COMPILER to Homebrew LLVM's clang++.")
endif()

include(GNUInstallDirs)
set(_faio_module_support "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
set(_faio_module_package_dir "${CMAKE_INSTALL_DATADIR}/cmake/faioModules")
set(_faio_module_source_dir "${CMAKE_INSTALL_DATADIR}/faio/cxx-module")

set(_faio_module_uring_default OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_faio_module_uring_default ON)
endif()
option(FAIO_ENABLE_IO_URING "Select the static Linux io_uring backend" ${_faio_module_uring_default})
option(FAIO_ENABLE_TLS "Build the optional TLS component" OFF)
option(FAIO_ENABLE_PROCESS "Build the optional asynchronous process component" OFF)
option(FAIO_COROUTINE_FRAME_USE_GLOBAL_NEW "Use global new/delete for task frames" OFF)
option(FAIO_MODULE_ENABLE_EXPERIMENTAL "Build the Linux GCC 16.1 experimental module" OFF)
option(FAIO_MODULE_EXPERIMENTAL_REFLECTION "Enable reflection, lifecycle and async entry in the module" ON)
option(FAIO_MODULE_EXPERIMENTAL_EXECUTION "Enable locked stdexec support in the module" OFF)
set(FAIO_SANITIZERS "" CACHE STRING "Sanitizers, e.g. address,undefined or thread")

find_package(Threads REQUIRED)
if(NOT TARGET spdlog::spdlog)
    find_package(spdlog CONFIG REQUIRED)
endif()
if(FAIO_ENABLE_IO_URING)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "FAIO_ENABLE_IO_URING is only supported on Linux")
    endif()
    list(PREPEND CMAKE_MODULE_PATH "${_faio_module_support}")
    find_package(Liburing REQUIRED)
endif()

function(faio_add_named_module target export_name)
    add_library("${target}" STATIC)
    add_library("faio::${export_name}" ALIAS "${target}")
    set_target_properties("${target}" PROPERTIES
        EXPORT_NAME "${export_name}"
        CXX_EXTENSIONS OFF
        CXX_SCAN_FOR_MODULES ON)
    target_compile_features("${target}" PUBLIC cxx_std_23)
    target_include_directories("${target}" PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
    if(WIN32)
        target_include_directories("${target}" PUBLIC
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include/faio/detail/io/platform/windows/compat>"
            "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/faio/detail/io/platform/windows/compat>")
    endif()
    target_sources("${target}" PUBLIC FILE_SET interfaces TYPE CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES ${ARGN})
endfunction()

function(faio_install_named_module target export_set)
    install(TARGETS "${target}" EXPORT "${export_set}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        FILE_SET interfaces DESTINATION "${_faio_module_source_dir}"
        FILE_SET entry_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
    install(EXPORT "${export_set}" NAMESPACE faio::
        DESTINATION "${_faio_module_package_dir}"
        CXX_MODULES_DIRECTORY modules)
endfunction()

faio_add_named_module(faio_module module
    faio.cppm
    core/common.cppm
    core/coroutine.cppm
    core/io.cppm
    core/net.cppm
    core/fs.cppm
    core/runtime.cppm
    core/sync.cppm
    core/time.cppm
    core/log.cppm)
set_property(TARGET faio_module PROPERTY INTERFACE_FAIO_VARIANT modules)
set_property(TARGET faio_module APPEND PROPERTY COMPATIBLE_INTERFACE_STRING FAIO_VARIANT)
target_link_libraries(faio_module PUBLIC Threads::Threads spdlog::spdlog)
target_compile_definitions(faio_module PUBLIC FAIO_HAS_IO_URING=$<BOOL:${FAIO_ENABLE_IO_URING}>)
if(MSVC)
    target_compile_options(faio_module PUBLIC /W4 /utf-8 /permissive- /Zc:__cplusplus)
else()
    target_compile_options(faio_module PUBLIC -Wall -Wextra -Wpedantic)
endif()
if(FAIO_ENABLE_IO_URING)
    target_link_libraries(faio_module PUBLIC Liburing::Liburing)
endif()
if(FAIO_COROUTINE_FRAME_USE_GLOBAL_NEW)
    target_compile_definitions(faio_module PUBLIC FAIO_COROUTINE_FRAME_USE_GLOBAL_NEW=1)
endif()
if(FAIO_SANITIZERS AND NOT MSVC)
    target_compile_options(faio_module PUBLIC "-fsanitize=${FAIO_SANITIZERS}" -fno-omit-frame-pointer)
    target_link_options(faio_module PUBLIC "-fsanitize=${FAIO_SANITIZERS}")
endif()
if(WIN32)
    target_compile_definitions(faio_module PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
    target_link_libraries(faio_module PUBLIC ws2_32 mswsock iphlpapi)
endif()

set(FAIO_MODULE_HAS_ALLOCATOR OFF)
if(EXISTS "${_faio_module_support}/FaioAllocator.cmake")
    include("${_faio_module_support}/FaioAllocator.cmake")
    faio_configure_allocator()
    target_link_libraries(faio_module PUBLIC faio::allocator)
    set(FAIO_MODULE_HAS_ALLOCATOR ON)
endif()

if(FAIO_ENABLE_TLS)
    find_package(OpenSSL 3.5 REQUIRED COMPONENTS SSL Crypto)
    target_sources(faio_module PUBLIC FILE_SET interfaces TYPE CXX_MODULES
        FILES extensions/tls.cppm)
    target_compile_definitions(faio_module PUBLIC FAIO_ENABLE_TLS=1)
    target_link_libraries(faio_module PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    add_library(faio_module_tls INTERFACE)
    add_library(faio::module_tls ALIAS faio_module_tls)
    set_target_properties(faio_module_tls PROPERTIES EXPORT_NAME module_tls)
    target_link_libraries(faio_module_tls INTERFACE faio_module)
endif()
if(FAIO_ENABLE_PROCESS)
    include("${_faio_module_support}/FaioProcessPlatform.cmake")
    faio_check_process_platform()
    target_sources(faio_module PUBLIC FILE_SET interfaces TYPE CXX_MODULES
        FILES extensions/process.cppm)
    target_compile_definitions(faio_module PUBLIC FAIO_ENABLE_PROCESS=1)
    add_library(faio_module_process INTERFACE)
    add_library(faio::module_process ALIAS faio_module_process)
    set_target_properties(faio_module_process PROPERTIES EXPORT_NAME module_process)
    target_link_libraries(faio_module_process INTERFACE faio_module)
    if(WIN32)
        target_link_libraries(faio_module PUBLIC bcrypt advapi32)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(faio_module PUBLIC _GNU_SOURCE)
    endif()
endif()

if(FAIO_MODULE_ENABLE_EXPERIMENTAL)
    include("${_faio_module_support}/FaioExperimentalToolchain.cmake")
    faio_check_experimental_toolchain("${FAIO_MODULE_EXPERIMENTAL_REFLECTION}")
    target_compile_features(faio_module PUBLIC cxx_std_26)
    target_compile_definitions(faio_module PUBLIC FAIO_ENABLE_EXPERIMENTAL=1
        FAIO_EXPERIMENTAL_HAS_REFLECTION=$<BOOL:${FAIO_MODULE_EXPERIMENTAL_REFLECTION}>
        FAIO_EXPERIMENTAL_TASK_LIFECYCLE=$<BOOL:${FAIO_MODULE_EXPERIMENTAL_REFLECTION}>
        FAIO_EXPERIMENTAL_HAS_EXECUTION=$<BOOL:${FAIO_MODULE_EXPERIMENTAL_EXECUTION}>)
    if(FAIO_MODULE_EXPERIMENTAL_REFLECTION)
        target_compile_options(faio_module PUBLIC -freflection)
    endif()
    if(FAIO_MODULE_EXPERIMENTAL_EXECUTION)
        include("${_faio_module_support}/FaioExperimentalDependencies.cmake")
        faio_find_stdexec()
        faio_check_experimental_execution_contract("${FAIO_STDEXEC_SOURCE_DIR}/include")
        target_include_directories(faio_module SYSTEM PUBLIC
            "$<BUILD_INTERFACE:${FAIO_STDEXEC_SOURCE_DIR}/include>"
            "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/faio-third-party/stdexec>")
    endif()
    faio_add_named_module(faio_module_experimental module_experimental experimental/experimental.cppm)
    target_sources(faio_module_experimental PUBLIC FILE_SET entry_headers TYPE HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/experimental/include"
        FILES experimental/include/faio-module/entry.hpp)
    target_link_libraries(faio_module_experimental PUBLIC faio_module)
endif()

if(FAIO_INSTALL)
    include(CMakePackageConfigHelpers)
    configure_package_config_file(
        faioModulesConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/faioModulesConfig.cmake"
        INSTALL_DESTINATION "${_faio_module_package_dir}")
    faio_install_named_module(faio_module faioModulesTargets)
    if(NOT TARGET faio)
        install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../include/faio" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
    endif()
    if(TARGET faio_module_experimental)
        faio_install_named_module(faio_module_experimental faioModulesExperimentalTargets)
        if(FAIO_MODULE_EXPERIMENTAL_EXECUTION)
            install(DIRECTORY "${FAIO_STDEXEC_SOURCE_DIR}/include/stdexec"
                "${FAIO_STDEXEC_SOURCE_DIR}/include/exec"
                DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/faio-third-party/stdexec")
            install(FILES "${FAIO_STDEXEC_SOURCE_DIR}/LICENSE.txt"
                DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/faio" RENAME stdexec-LICENSE.txt)
        endif()
    endif()
    if(FAIO_MODULE_HAS_ALLOCATOR)
        install(FILES "${_faio_module_support}/FaioAllocator.cmake" "${_faio_module_support}/FindTCMalloc.cmake"
            DESTINATION "${_faio_module_package_dir}")
    endif()
    if(FAIO_ENABLE_IO_URING)
        install(FILES "${_faio_module_support}/FindLiburing.cmake" DESTINATION "${_faio_module_package_dir}")
    endif()
    # Consumers that import the C++23 std module need the same pre-project() switch.
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/FaioImportStd.cmake"
        DESTINATION "${_faio_module_package_dir}")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/faioModulesConfig.cmake"
        DESTINATION "${_faio_module_package_dir}")
endif()
