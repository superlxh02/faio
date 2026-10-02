cmake_minimum_required(VERSION 3.20)
# 默认使用 GCC；显式传入工具链/编译器时保留调用方选择。
if(NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED CMAKE_TOOLCHAIN_FILE AND "$ENV{CXX}" STREQUAL "")
    set(CMAKE_CXX_COMPILER g++ CACHE STRING "Default C++ compiler")
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "Release flags")
project(faio LANGUAGES CXX)
option(FAIO_BUILD_EXAMPLES "Build faio examples" ON)
option(FAIO_BUILD_BENCHMARKS "Build faio benchmarks" ON)
option(FAIO_BUILD_TESTS "Build faio tests" ON)
option(FAIO_INSTALL "Install faio headers and CMake package" ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
	set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

# 性能测试统一默认基线为 GCC -O2，不让 CMake 的 Release -O3 或 LTO 混入比较。
# 独立实验仍可通过缓存参数覆盖，实验结果必须注明实际配置。
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO OFF)

find_package(PkgConfig REQUIRED)
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
	set(_vcpkg_pkgconfig_dir "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
	set(ENV{PKG_CONFIG_PATH} "${_vcpkg_pkgconfig_dir}/lib/pkgconfig:${_vcpkg_pkgconfig_dir}/share/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif()
pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing)
find_package(Threads REQUIRED)
find_package(spdlog CONFIG REQUIRED)
include(GNUInstallDirs)

add_library(faio INTERFACE)
add_library(faio::faio ALIAS faio)
target_compile_features(faio INTERFACE cxx_std_23)
target_include_directories(faio INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(faio INTERFACE PkgConfig::LIBURING Threads::Threads spdlog::spdlog)

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
    install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/faio")
endif()

if(FAIO_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
if(FAIO_BUILD_BENCHMARKS)
    add_subdirectory(benchmark)
endif()
if(FAIO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
