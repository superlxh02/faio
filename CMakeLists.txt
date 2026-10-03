# =============================================================================
# faio 根构建入口：公共选项、第三方库、编译器与平台设置、安装导出、开发目标。
# faio 是头文件库，INTERFACE 设置会传递给链接 faio::faio 的消费目标。
# =============================================================================
cmake_minimum_required(VERSION 3.20)

# 非 Windows 的 Release 默认基线为 -O2，须在 project 初始化选项前设置。
# CACHE 不带 FORCE：保留命令行、preset 或工具链明确传入的 Release 参数。
# Windows 的 /O2 或 -O2 由编译器 preset 指定，不能在这里混用参数风格。
if(NOT WIN32)
    set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "Release flags")
endif()

# 工具链由调用方或 preset 选择；不覆盖 CMake 的平台默认编译器。
project(faio LANGUAGES CXX)

# =============================================================================
# 一、构建选项与通用设置
# =============================================================================

# Windows GCC 的 native TLS 检查与对象适配必须早于第三方及本项目目标，
# 后续目标才能继承编译 launcher；MSVC、clang-cl 和非 Windows 平台直接返回。
include(cmake/FaioMinGW.cmake)
faio_enable_mingw_tls_workaround()

# 四个独立开关分别控制示例、性能基准、测试和安装规则。
option(FAIO_BUILD_EXAMPLES "Build faio examples" ON)
option(FAIO_BUILD_BENCHMARKS "Build faio benchmarks" ON)
option(FAIO_BUILD_TESTS "Build faio tests" ON)
option(FAIO_INSTALL "Install faio headers and CMake package" ON)

# 按目标平台决定后端默认值：Linux 默认编译 epoll/io_uring 双后端；
# Windows 使用 IOCP，macOS 使用 kqueue，二者不依赖 Linux 专用库。
set(FAIO_IO_URING_DEFAULT OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(FAIO_IO_URING_DEFAULT ON)
endif()
option(FAIO_ENABLE_IO_URING "Build the native Linux io_uring backend alongside epoll" ${FAIO_IO_URING_DEFAULT})

# 后续 C++ 目标使用 C++23，禁止标准降级和语言扩展；同时输出编译命令数据库。
# faio 的公共目标还会声明 cxx_std_23，使安装消费方获得同一语言要求。
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 留空关闭检测；GNU 风格可填写检测器列表，Windows 已验证 clang-cl + address。
set(FAIO_SANITIZERS "" CACHE STRING "Sanitizers, e.g. address,undefined or thread")

# 单配置生成器缺省使用 Release；不覆盖用户设置或 Visual Studio 等多配置生成器。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

# 默认关闭 Release/RelWithDebInfo 的跨过程优化，保持 benchmark 优化基线一致。
# MinGW 的精确 TLS alias 适配依赖普通 COFF 对象，不支持 GCC LTO 对象。
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO OFF)

# =============================================================================
# 二、第三方库集成
# =============================================================================

# liburing 是 Linux 原生后端的条件依赖；在其他平台开启属于配置错误。
# 自定义查找模块同时检查头文件、库及所需 API 的编译链接能力。
if(FAIO_ENABLE_IO_URING)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "FAIO_ENABLE_IO_URING is only supported on Linux")
    endif()
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
    find_package(Liburing REQUIRED)
endif()

# 线程库由 CMake 选择平台实现，不手工向 Windows 添加 pthread 参数。
find_package(Threads REQUIRED)

# spdlog 为公开依赖：工作区模式从固定版本源码构建，否则查找 CONFIG 安装包。
# 模块中的 GoogleTest 和 Asio 入口分别由测试及 benchmark 子目录按需调用。
include(cmake/FaioDependencies.cmake)
faio_find_spdlog()

# 标准安装路径支持调用方覆盖 CMAKE_INSTALL_INCLUDEDIR/DATADIR 等变量。
include(GNUInstallDirs)

# =============================================================================
# 三、公共目标与编译器设置
# =============================================================================

# INTERFACE 库不生成 faio 二进制；别名统一源码构建和安装消费的链接名称。
add_library(faio INTERFACE)
add_library(faio::faio ALIAS faio)
target_compile_features(faio INTERFACE cxx_std_23)

if(MSVC)
    # MSVC 条件表示 Microsoft ABI，包含 cl.exe 和 clang-cl.exe。
    # clang-cl 同样使用 / 风格设置，其 Clang ID 不代表应走 GNU 参数分支。
    target_compile_options(faio INTERFACE
        /W4              # MSVC/clang-cl：第四级警告，保留已验证的诊断基线。
        /utf-8           # MSVC/clang-cl：源文件与执行字符集均为 UTF-8，支持中文。
        /permissive-     # MSVC/clang-cl：严格标准一致性模式。
        /Zc:__cplusplus) # MSVC/clang-cl：__cplusplus 正确反映 C++23 语言模式。
else()
    # GCC（Linux/MinGW）、clang++ 和 AppleClang 共用 GNU 风格警告，合并相同设置。
    # clang-cl 已由 MSVC 分支处理；MinGW 的 TLS 适配由前面的模块独立完成。
    # 保留既有 GNU 风格后备配置，未知编译器由调用方确认兼容性。
    target_compile_options(faio INTERFACE
        -Wall            # GCC/MinGW/Clang/AppleClang：启用常用警告。
        -Wextra          # GCC/MinGW/Clang/AppleClang：增加补充诊断。
        -Wpedantic)      # GCC/MinGW/Clang/AppleClang：检查标准一致性。
endif()

# 内存检测默认关闭，只在明确设置 FAIO_SANITIZERS 时加入检测开销。
if(FAIO_SANITIZERS)
    if(MSVC)
        if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND FAIO_SANITIZERS STREQUAL "address")
            # clang-cl 由 CMake 直接调用 link.exe，需显式提供 ASan 运行库和 thunk。
            # 查询当前工具链资源目录，防止误用 PATH 中其他 LLVM 版本的运行库。
            execute_process(
                COMMAND "${CMAKE_CXX_COMPILER}" -print-resource-dir
                OUTPUT_VARIABLE _faio_clang_resource
                OUTPUT_STRIP_TRAILING_WHITESPACE
                COMMAND_ERROR_IS_FATAL ANY)
            file(TO_CMAKE_PATH "${_faio_clang_resource}" _faio_clang_resource)

            # 使用已验证的 x86-64 动态运行库，不从系统其他目录回退查找。
            find_library(_faio_asan_runtime NAMES clang_rt.asan_dynamic-x86_64
                PATHS "${_faio_clang_resource}/lib/windows" NO_DEFAULT_PATH REQUIRED)
            find_library(_faio_asan_thunk NAMES clang_rt.asan_dynamic_runtime_thunk-x86_64
                PATHS "${_faio_clang_resource}/lib/windows" NO_DEFAULT_PATH REQUIRED)
            # /fsanitize=address：地址检测；/Oy-：保留帧指针；/Z7：对象内调试信息。
            # STL 的 ASan 容器注解要求依赖与消费目标使用相同的检测配置。
            target_compile_options(faio INTERFACE /fsanitize=address /Oy- /Z7)
            target_compile_options(spdlog PRIVATE /fsanitize=address /Oy- /Z7)

            # spdlog 已创建，故单独设置；目录选项覆盖之后创建的 GoogleTest 等目标。
            add_compile_options(/fsanitize=address /Oy- /Z7)
            target_link_libraries(faio INTERFACE "${_faio_asan_runtime}" "${_faio_asan_thunk}")

            # wholearchive 保留 thunk；禁用增量链接，并生成诊断需要的 PDB。
            target_link_options(faio INTERFACE "/wholearchive:${_faio_asan_thunk}" /INCREMENTAL:NO /DEBUG)
        else()
            message(FATAL_ERROR "Windows sanitizer validation supports clang-cl with FAIO_SANITIZERS=address")
        endif()
    else()
        # GCC、MinGW 和 GNU 风格 Clang：编译与链接传入同一检测器列表。
        # 保留帧指针用于诊断；实际检测器可用性由所选工具链决定。
        target_compile_options(faio INTERFACE -fsanitize=${FAIO_SANITIZERS} -fno-omit-frame-pointer)
        target_link_options(faio INTERFACE -fsanitize=${FAIO_SANITIZERS})
    endif()
endif()

# =============================================================================
# 四、公开依赖、头文件路径与平台设置
# =============================================================================

# 构建树引用源码绝对路径，安装树使用相对安装前缀路径，确保导出包可迁移。
target_include_directories(faio INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(faio INTERFACE Threads::Threads spdlog::spdlog)

# 所有消费目标获得一致的后端编译开关；只有启用原生后端才传递 liburing。
target_compile_definitions(faio INTERFACE FAIO_HAS_IO_URING=$<BOOL:${FAIO_ENABLE_IO_URING}>)
if(FAIO_ENABLE_IO_URING)
    target_link_libraries(faio INTERFACE Liburing::Liburing)
endif()

if(WIN32)
    # Windows 公共系统库：Winsock、Winsock 扩展接口、网卡信息查询。
    # 这是 MSVC/clang-cl/MinGW 共同的平台依赖，与编译器参数分支分开维护。
    target_link_libraries(faio INTERFACE ws2_32 mswsock iphlpapi)

    # NOMINMAX 避免 min/max 宏污染 C++；LEAN_AND_MEAN 减少 Windows 头文件内容。
    # _WIN32_WINNT=0x0A00 声明 Windows 10 API 基线，与 IOCP 实现要求一致。
    target_compile_definitions(faio INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)

    # common/concepts.hpp 的 POSIX 类型名称在 Windows 由 I/O 兼容层提供。
    # 分别配置源码构建与安装消费路径，保持公共协程及 runtime 模块不变。
    target_include_directories(faio INTERFACE
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/faio/detail/io/platform/windows_compat>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/faio/detail/io/platform/windows_compat>")
endif()

# Linux epoll/可选 io_uring 与 macOS kqueue 由已有平台源码选择。
# 不向这两个平台附加 Windows 系统库、宏或兼容头路径。

# =============================================================================
# 五、编译器与标准库能力检查
# =============================================================================

# 同时检查 expected、format 和 coroutine；执行编译链接，不运行探测程序。
# 可在配置阶段诊断系统 AppleClang 标准库不足，不能只按编译器版本判断。
include(CheckCXXSourceCompiles)
check_cxx_source_compiles(
    "#include <expected>\n#include <format>\n#include <coroutine>\nint main() { std::expected<int,int> value{1}; return std::format(\"{}\", *value).empty(); }"
    FAIO_HAS_CXX23_LIBRARY)
if(NOT FAIO_HAS_CXX23_LIBRARY)
    message(FATAL_ERROR "faio requires C++23 std::expected/std::format and C++20 coroutines; on macOS use the macos-clang23 preset")
endif()

# =============================================================================
# 六、安装与 CMake 包导出
# =============================================================================

if(FAIO_INSTALL)
    include(CMakePackageConfigHelpers)
    set(FAIO_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/cmake/faio")

    # 生成可迁移的包配置，消费方 find_package(faio CONFIG) 会重新查找依赖。
    configure_package_config_file(
        cmake/faioConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/faioConfig.cmake"
        INSTALL_DESTINATION "${FAIO_CMAKE_INSTALL_DIR}")

    # 安装头文件，并导出 faio::faio 的传递编译、平台宏及链接要求。
    install(TARGETS faio EXPORT faioTargets)
    install(DIRECTORY include/faio DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
    install(EXPORT faioTargets
        FILE faioTargets.cmake
        NAMESPACE faio::
        DESTINATION "${FAIO_CMAKE_INSTALL_DIR}")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/faioConfig.cmake"
        DESTINATION "${FAIO_CMAKE_INSTALL_DIR}")

    # 查找模块与 MinGW 的 CMake/Python 工具需要随包安装，供消费方独立使用。
    # faio_mingw_compile.py 是编译链的必要组成，不能作为临时脚本清理。
    install(FILES cmake/FindLiburing.cmake DESTINATION "${FAIO_CMAKE_INSTALL_DIR}")
    install(FILES cmake/FaioMinGW.cmake cmake/faio_mingw_compile.py DESTINATION "${FAIO_CMAKE_INSTALL_DIR}")
    install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/faio")
endif()

# =============================================================================
# 七、按开关加入示例、benchmark 与测试
# =============================================================================

# 根目录启用 CTest，才能从构建根发现子目录注册的测试。
if(FAIO_BUILD_TESTS)
    enable_testing()
endif()

# 保持目录顺序：示例 -> benchmark -> 测试；每个子目录只管理自己的目标。
if(FAIO_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
if(FAIO_BUILD_BENCHMARKS)
    add_subdirectory(benchmark)
endif()
if(FAIO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
