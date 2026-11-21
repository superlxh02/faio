#[[
@file cmake/FaioDependencies.cmake
@brief 查找 spdlog、GoogleTest 和 Asio 依赖。
]]
option(FAIO_USE_WORKSPACE_DEPS "Use pinned dependencies bootstrapped next to the workspace" ${WIN32})

get_filename_component(_faio_workspace_parent "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(FAIO_DEPENDENCY_ROOT "${_faio_workspace_parent}/faio-deps" CACHE PATH "Pinned dependency source and tool cache")

#[[ @brief 查找已安装或工作区固定版本的 spdlog。 ]]
function(faio_find_spdlog)
    if(FAIO_USE_WORKSPACE_DEPS)

        if(NOT EXISTS "${FAIO_DEPENDENCY_ROOT}/src/spdlog/CMakeLists.txt")
            message(FATAL_ERROR "Run scripts/bootstrap_windows.ps1 first, or set FAIO_USE_WORKSPACE_DEPS=OFF and provide spdlog via CMAKE_PREFIX_PATH")
        endif()

        set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
        set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)

        set(SPDLOG_INSTALL ${FAIO_INSTALL} CACHE BOOL "" FORCE)
        add_subdirectory("${FAIO_DEPENDENCY_ROOT}/src/spdlog" "${CMAKE_BINARY_DIR}/dependencies/spdlog")
    else()

        find_package(spdlog CONFIG REQUIRED)
    endif()
endfunction()

#[[ @brief 查找单元测试所需的 GoogleTest。 ]]
function(faio_find_gtest)
    if(FAIO_USE_WORKSPACE_DEPS)
        if(NOT EXISTS "${FAIO_DEPENDENCY_ROOT}/src/googletest/CMakeLists.txt")
            message(FATAL_ERROR "Missing pinned GoogleTest; run scripts/bootstrap_windows.ps1")
        endif()

        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)

        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        add_subdirectory("${FAIO_DEPENDENCY_ROOT}/src/googletest" "${CMAKE_BINARY_DIR}/dependencies/googletest")

        if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")

            target_compile_options(gtest PRIVATE -Wno-error=character-conversion)
        endif()
    else()
        find_package(GTest CONFIG REQUIRED)
    endif()
endfunction()

#[[ @brief 查找 Asio 性能基准依赖。 ]]
function(faio_find_asio)
    if(FAIO_USE_WORKSPACE_DEPS)

        set(ASIO_INCLUDE_DIR "${FAIO_DEPENDENCY_ROOT}/src/asio/asio/include")
    else()

        find_package(asio CONFIG QUIET)
        if(NOT TARGET asio::asio)
            find_path(ASIO_INCLUDE_DIR asio.hpp)
        endif()
    endif()

    if(NOT TARGET asio::asio AND EXISTS "${ASIO_INCLUDE_DIR}/asio.hpp")
        add_library(asio::asio INTERFACE IMPORTED GLOBAL)

        set_target_properties(asio::asio PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${ASIO_INCLUDE_DIR}"
            INTERFACE_COMPILE_DEFINITIONS ASIO_STANDALONE)
    endif()
endfunction()
