#[[
@file CMakeLists.txt
@brief 选择 faio 的头文件和模块构建入口。
]]
cmake_minimum_required(VERSION 3.20)

# `import std;` 的开关必须在 project() 之前生效；只由模块示例使用。
include(cmake/FaioImportStd.cmake)

if(NOT WIN32)
    set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "Release flags")
endif()

project(faio LANGUAGES CXX)

option(FAIO_BUILD_HEADERS "Build the faio header-only targets" ON)
option(FAIO_BUILD_MODULES "Build the C++ module targets" OFF)
option(FAIO_BUILD_EXAMPLES "Build faio examples" ON)
option(FAIO_BUILD_BENCHMARKS "Build faio benchmarks" ON)
option(FAIO_BUILD_TESTS "Build faio tests" ON)
option(FAIO_INSTALL "Install the enabled faio packages" ON)

if(NOT FAIO_BUILD_HEADERS AND NOT FAIO_BUILD_MODULES)
    message(FATAL_ERROR "Enable FAIO_BUILD_HEADERS or FAIO_BUILD_MODULES")
endif()
if(FAIO_BUILD_MODULES AND CMAKE_VERSION VERSION_LESS 3.28)
    message(FATAL_ERROR "FAIO_BUILD_MODULES requires CMake 3.28 or newer")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO OFF)

include(CheckCXXSourceCompiles)
check_cxx_source_compiles(
    "struct base { int value(this auto const&) { return 0; } }; struct derived : base {}; int main() { return derived{}.value(); }"
    FAIO_HAS_DEDUCING_THIS)
if(NOT FAIO_HAS_DEDUCING_THIS)
    message(FATAL_ERROR "faio requires C++23 explicit object parameters (deducing this); use a compiler implementing this feature")
endif()
check_cxx_source_compiles(
    "#include <expected>\n#include <format>\n#include <coroutine>\nint main() { std::expected<int,int> value{1}; return std::format(\"{}\", *value).empty(); }"
    FAIO_HAS_CXX23_LIBRARY)
if(NOT FAIO_HAS_CXX23_LIBRARY)
    message(FATAL_ERROR "faio requires C++23 std::expected/std::format and C++20 coroutines; on macOS use the mac-main-release or mac-main-debug preset")
endif()

include(GNUInstallDirs)
if(FAIO_BUILD_TESTS)
    enable_testing()
endif()
if(FAIO_BUILD_HEADERS)
    include(cmake/FaioHeaders.cmake)
endif()
if(FAIO_BUILD_MODULES)
    add_subdirectory(cxx-module)
    if(FAIO_BUILD_TESTS OR FAIO_BUILD_EXAMPLES)
        add_subdirectory(tests/modules)
    endif()
endif()
if(FAIO_INSTALL)
    install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/faio")
endif()
