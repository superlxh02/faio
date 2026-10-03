# 第三方依赖的统一入口。
#
# 本模块只选择已存在的源码或安装包，不在 CMake 配置过程中联网下载。
# Windows 默认使用 scripts/bootstrap_windows.ps1 校验并解压的固定版本源码；
# 其他平台默认使用 find_package，可显式开启 FAIO_USE_WORKSPACE_DEPS 改用源码。
# 每个构建目录使用自己的编译器重新编译依赖，避免混用 MSVC 与 MinGW 的 ABI。

# 缓存选项允许用户在工作区源码集成和已有依赖安装包之间切换。
option(FAIO_USE_WORKSPACE_DEPS "Use pinned dependencies bootstrapped next to the workspace" ${WIN32})

# 默认依赖缓存放在仓库相邻目录，不将第三方源码复制到 faio 仓库内。
get_filename_component(_faio_workspace_parent "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(FAIO_DEPENDENCY_ROOT "${_faio_workspace_parent}/faio-deps" CACHE PATH "Pinned dependency source and tool cache")

# 集成日志依赖，向调用方提供 spdlog 的原生 CMake 目标。
# 源码集成时由当前工具链生成二进制；安装包模式通过 CMAKE_PREFIX_PATH 查找。
function(faio_find_spdlog)
    if(FAIO_USE_WORKSPACE_DEPS)
        # 缺少缓存时立即给出修复方式，避免后续出现含糊的目标或头文件错误。
        if(NOT EXISTS "${FAIO_DEPENDENCY_ROOT}/src/spdlog/CMakeLists.txt")
            message(FATAL_ERROR "Run scripts/bootstrap_windows.ps1 first, or set FAIO_USE_WORKSPACE_DEPS=OFF and provide spdlog via CMAKE_PREFIX_PATH")
        endif()

        # 只构建实际日志库，第三方自己的示例和测试不进入 faio 的构建矩阵。
        set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
        set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)

        # faio 安装包需要 spdlog 依赖；是否生成其安装规则跟随 faio 的安装开关。
        set(SPDLOG_INSTALL ${FAIO_INSTALL} CACHE BOOL "" FORCE)
        add_subdirectory("${FAIO_DEPENDENCY_ROOT}/src/spdlog" "${CMAKE_BINARY_DIR}/dependencies/spdlog")
    else()
        # CONFIG 模式使用 spdlog 自带的导出目标，不自行猜测库名与传递依赖。
        find_package(spdlog CONFIG REQUIRED)
    endif()
endfunction()

# 集成测试框架，只在调用方需要测试时执行。
# 函数内部的缓存变量配置第三方子项目，gtest 目标随后供测试目录链接。
function(faio_find_gtest)
    if(FAIO_USE_WORKSPACE_DEPS)
        if(NOT EXISTS "${FAIO_DEPENDENCY_ROOT}/src/googletest/CMakeLists.txt")
            message(FATAL_ERROR "Missing pinned GoogleTest; run scripts/bootstrap_windows.ps1")
        endif()

        # 测试依赖不随 faio 安装，也不构建当前测试未使用的 GoogleMock。
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)

        # MSVC 和 clang-cl 使用 Microsoft CRT，此选项使 GoogleTest 跟随共享 CRT。
        # GoogleTest 在非 MSVC 工具链中自行忽略这一 Microsoft CRT 专用选项。
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        add_subdirectory("${FAIO_DEPENDENCY_ROOT}/src/googletest" "${CMAKE_BINARY_DIR}/dependencies/googletest")

        # 编译器 ID 为 Clang 且平台为 Windows 时，覆盖已验证的 clang-cl 构建。
        if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            # GoogleTest 1.17 的 UTF-8 打印分支触发 Clang 22 新警告。
            # PRIVATE 只取消 gtest 该警告的 -Werror，不降低 faio 或消费方的诊断级别。
            target_compile_options(gtest PRIVATE -Wno-error=character-conversion)
        endif()
    else()
        find_package(GTest CONFIG REQUIRED)
    endif()
endfunction()

# 为 Asio 对照基准准备头文件目标，不构建 Asio 的示例或任何二进制库。
# 如果已有 asio::asio，保留其配置；找不到 Asio 时也不在本函数强制报错，
# 是否跳过相关基准由调用方决定，保持 Asio 对核心库的可选依赖性质。
function(faio_find_asio)
    if(FAIO_USE_WORKSPACE_DEPS)
        # 固定源码包的仓库层级为 src/asio/asio/include。
        set(ASIO_INCLUDE_DIR "${FAIO_DEPENDENCY_ROOT}/src/asio/asio/include")
    else()
        # 优先使用安装包的目标；没有该目标时，再查找独立头文件安装位置。
        find_package(asio CONFIG QUIET)
        if(NOT TARGET asio::asio)
            find_path(ASIO_INCLUDE_DIR asio.hpp)
        endif()
    endif()

    # 裸头文件安装需要补充目标，GLOBAL 让其他目录中的基准也能引用它。
    if(NOT TARGET asio::asio AND EXISTS "${ASIO_INCLUDE_DIR}/asio.hpp")
        add_library(asio::asio INTERFACE IMPORTED GLOBAL)

        # standalone 模式使用 Asio 自身的头文件实现，避免引入 Boost.Asio 配置。
        set_target_properties(asio::asio PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${ASIO_INCLUDE_DIR}"
            INTERFACE_COMPILE_DEFINITIONS ASIO_STANDALONE)
    endif()
endfunction()
