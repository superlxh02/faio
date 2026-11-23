cmake_minimum_required(VERSION 3.28)

# 独立示例工程里同样在 project() 之前打开 `import std;` 的开关：先找 faio
# 源码树里的 helper，安装包模式下再在 CMAKE_PREFIX_PATH 里找 faioModules 附带的副本。
set(FAIO_IMPORT_STD_MODULE "${CMAKE_CURRENT_LIST_DIR}/../../cmake/FaioImportStd.cmake")
if(NOT EXISTS "${FAIO_IMPORT_STD_MODULE}")
    find_file(FAIO_IMPORT_STD_MODULE FaioImportStd.cmake
        PATHS ${CMAKE_PREFIX_PATH}
        PATH_SUFFIXES lib/cmake/faioModules lib64/cmake/faioModules
                      share/cmake/faioModules)
endif()
if(EXISTS "${FAIO_IMPORT_STD_MODULE}")
    include("${FAIO_IMPORT_STD_MODULE}")
endif()

project(faio_module_example LANGUAGES CXX)

# This example always writes `import std;`, so refuse toolchains that cannot
# build the C++23 standard library module instead of failing at compile time.
if(NOT COMMAND faio_import_std_supported)
    message(FATAL_ERROR
        "faio: 找不到 FaioImportStd.cmake；源码接入时需要 faio 仓库的 cmake/ 目录，"
        "安装包接入时需要 faioModules 包里的同文件。")
endif()
set(FAIO_IMPORT_STD_SUPPORTED FALSE)
faio_import_std_supported(FAIO_IMPORT_STD_SUPPORTED)
if(NOT FAIO_IMPORT_STD_SUPPORTED)
    message(FATAL_ERROR
        "faio: examples/module 默认 import std;，需要 GCC 16.1 及以上；"
        "请用 -DCMAKE_CXX_COMPILER=g++-16 配置。")
endif()

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
if(COMMAND faio_enable_import_std)
    faio_enable_import_std(quickstart_module)
endif()
