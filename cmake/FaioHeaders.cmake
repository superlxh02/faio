#[[
@file cmake/FaioHeaders.cmake
@brief 纯头文件目标、依赖、开发目标及安装导出。
]]
include(cmake/FaioMinGW.cmake)
faio_enable_mingw_tls_workaround()

option(FAIO_COROUTINE_FRAME_USE_GLOBAL_NEW
    "Use global new/delete for task frames (allows allocator interposition such as tcmalloc)" OFF)

set(FAIO_IO_URING_DEFAULT OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(FAIO_IO_URING_DEFAULT ON)
endif()
option(FAIO_ENABLE_IO_URING "Select the static Linux io_uring backend (OFF selects epoll)" ${FAIO_IO_URING_DEFAULT})
set(FAIO_SANITIZERS "" CACHE STRING "Sanitizers, e.g. address,undefined or thread")

if(FAIO_ENABLE_IO_URING)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "FAIO_ENABLE_IO_URING is only supported on Linux")
    endif()
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
    find_package(Liburing REQUIRED)
endif()

find_package(Threads REQUIRED)

include(cmake/FaioDependencies.cmake)
faio_find_spdlog()

include(cmake/FaioAllocator.cmake)
faio_configure_allocator()

add_library(faio INTERFACE)
add_library(faio::faio ALIAS faio)
set_property(TARGET faio PROPERTY INTERFACE_FAIO_VARIANT headers)
set_property(TARGET faio APPEND PROPERTY COMPATIBLE_INTERFACE_STRING FAIO_VARIANT)
target_compile_features(faio INTERFACE cxx_std_23)
if(FAIO_COROUTINE_FRAME_USE_GLOBAL_NEW)
    target_compile_definitions(faio INTERFACE FAIO_COROUTINE_FRAME_USE_GLOBAL_NEW=1)
endif()

if(MSVC)

    target_compile_options(faio INTERFACE
        /W4
        /utf-8
        /permissive-
        /Zc:__cplusplus)
else()

    target_compile_options(faio INTERFACE
        -Wall
        -Wextra
        -Wpedantic)
endif()

if(FAIO_SANITIZERS)
    if(MSVC)
        if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND FAIO_SANITIZERS STREQUAL "address")

            execute_process(
                COMMAND "${CMAKE_CXX_COMPILER}" -print-resource-dir
                OUTPUT_VARIABLE _faio_clang_resource
                OUTPUT_STRIP_TRAILING_WHITESPACE
                COMMAND_ERROR_IS_FATAL ANY)
            file(TO_CMAKE_PATH "${_faio_clang_resource}" _faio_clang_resource)

            find_library(_faio_asan_runtime NAMES clang_rt.asan_dynamic-x86_64
                PATHS "${_faio_clang_resource}/lib/windows" NO_DEFAULT_PATH REQUIRED)
            find_library(_faio_asan_thunk NAMES clang_rt.asan_dynamic_runtime_thunk-x86_64
                PATHS "${_faio_clang_resource}/lib/windows" NO_DEFAULT_PATH REQUIRED)

            target_compile_options(faio INTERFACE /fsanitize=address /Oy- /Z7)
            target_compile_options(spdlog PRIVATE /fsanitize=address /Oy- /Z7)

            add_compile_options(/fsanitize=address /Oy- /Z7)
            target_link_libraries(faio INTERFACE "${_faio_asan_runtime}" "${_faio_asan_thunk}")

            target_link_options(faio INTERFACE "/wholearchive:${_faio_asan_thunk}" /INCREMENTAL:NO /DEBUG)
        else()
            message(FATAL_ERROR "Windows sanitizer validation supports clang-cl with FAIO_SANITIZERS=address")
        endif()
    else()

        target_compile_options(faio INTERFACE -fsanitize=${FAIO_SANITIZERS} -fno-omit-frame-pointer)

        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
           AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 23
           AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 24)
            target_compile_options(faio INTERFACE "$<$<CONFIG:Debug>:-O1>")
        endif()
        target_link_options(faio INTERFACE -fsanitize=${FAIO_SANITIZERS})
    endif()
endif()

target_include_directories(faio INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(faio INTERFACE Threads::Threads spdlog::spdlog faio::allocator)

target_compile_definitions(faio INTERFACE FAIO_HAS_IO_URING=$<BOOL:${FAIO_ENABLE_IO_URING}>)
if(FAIO_ENABLE_IO_URING)
    target_link_libraries(faio INTERFACE Liburing::Liburing)
endif()

if(WIN32)

    target_link_libraries(faio INTERFACE ws2_32 mswsock iphlpapi)

    target_compile_definitions(faio INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)

    target_include_directories(faio INTERFACE
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/faio/detail/io/platform/windows/compat>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/faio/detail/io/platform/windows/compat>")
endif()

include(cmake/FaioExperimental.cmake)
include(cmake/FaioProtocols.cmake)
include(cmake/FaioProcess.cmake)

if(FAIO_INSTALL)
    include(CMakePackageConfigHelpers)
    set(FAIO_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/cmake/faio")

    configure_package_config_file(
        cmake/faioConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/faioConfig.cmake"
        INSTALL_DESTINATION "${FAIO_CMAKE_INSTALL_DIR}")

    install(TARGETS faio EXPORT faioTargets)
    install(DIRECTORY include/faio DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
    install(EXPORT faioTargets
        FILE faioTargets.cmake
        NAMESPACE faio::
        DESTINATION "${FAIO_CMAKE_INSTALL_DIR}")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/faioConfig.cmake"
        DESTINATION "${FAIO_CMAKE_INSTALL_DIR}")

    install(FILES cmake/FindLiburing.cmake cmake/FindTCMalloc.cmake cmake/FaioAllocator.cmake
        DESTINATION "${FAIO_CMAKE_INSTALL_DIR}")
    install(FILES cmake/FaioMinGW.cmake cmake/faio_mingw_compile.py DESTINATION "${FAIO_CMAKE_INSTALL_DIR}")
endif()

if(FAIO_BUILD_TESTS)
    faio_find_gtest()
endif()

if(FAIO_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
if(FAIO_BUILD_BENCHMARKS)
    add_subdirectory(benchmark)
endif()
if(FAIO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
if(FAIO_ENABLE_EXPERIMENTAL)
    if(FAIO_BUILD_EXAMPLES)
        add_subdirectory(examples/experimental)
    endif()
    if(FAIO_BUILD_TESTS)
        add_subdirectory(tests/experimental)
    endif()
endif()
