cmake_minimum_required(VERSION 3.28)

project(faio_module_example LANGUAGES CXX)

option(FAIO_MODULE_EXAMPLE_USE_PACKAGE "Use an installed faioModules package" OFF)

if(FAIO_MODULE_EXAMPLE_USE_PACKAGE)
    find_package(faioModules CONFIG REQUIRED)
else()
    set(FAIO_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "Path to the faio source checkout")
    option(FAIO_ENABLE_IO_URING "Use io_uring instead of epoll on Linux" OFF)
    add_subdirectory("${FAIO_SOURCE_DIR}/cxx-module" "${CMAKE_CURRENT_BINARY_DIR}/faio")
endif()

add_executable(quickstart_module quickstart.cpp)
set_target_properties(quickstart_module PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_SCAN_FOR_MODULES ON)
target_link_libraries(quickstart_module PRIVATE faio::module)
