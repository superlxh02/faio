# 查找 Linux 原生 io_uring 后端的用户态依赖，并导出 Liburing::Liburing。
#
# 本模块不主动启用任何后端：根构建和安装包只在 FAIO_ENABLE_IO_URING 打开时调用。
# 系统开发包、vcpkg 或自定义安装前缀均可提供 liburing.h 与 uring 库。
# pkg-config 只提供路径提示与版本信息，仍使用 CMake 的标准查找规则找到实际文件。

# pkg-config 可缺省；没有该工具时，后面的 find_path/find_library 仍可查找依赖。
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_FAIO_LIBURING QUIET liburing)
endif()

# 缓存变量允许用户指定位置，提示路径和标准 CMake 搜索路径均参与查找。
find_path(Liburing_INCLUDE_DIR liburing.h HINTS ${PC_FAIO_LIBURING_INCLUDE_DIRS})
find_library(Liburing_LIBRARY NAMES uring HINTS ${PC_FAIO_LIBURING_LIBRARY_DIRS})

# 只有 pkg-config 提供版本号时才填充版本信息，不由路径或文件名推断版本。
set(Liburing_VERSION "${PC_FAIO_LIBURING_VERSION}")

# 配置与安装消费阶段都验证当前头文件和库可编译、链接所需的原生 API。
# 这是编译探测，不运行代码；内核 opcode 是否可用仍由运行时 probe 判断。
# 不能把新版用户态库等同于宿主内核具备对应操作能力。
if(Liburing_INCLUDE_DIR AND Liburing_LIBRARY)
    include(CheckCXXSourceCompiles)
    include(CMakePushCheckState)

    # 隔离探测的 include/link 配置，避免继承其他检查状态或污染后续检查。
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_INCLUDES "${Liburing_INCLUDE_DIR}")
    set(CMAKE_REQUIRED_LIBRARIES "${Liburing_LIBRARY}")

    # 同时引用 ftruncate、accept 非阻塞标志和 enter2，防止仅头文件新而库过旧。
    check_cxx_source_compiles(
        "#include <liburing.h>\nint main() { io_uring_sqe sqe{}; io_uring_prep_ftruncate(&sqe, -1, 0); io_uring_prep_accept(&sqe, -1, nullptr, nullptr, IORING_ACCEPT_DONTWAIT); return io_uring_enter2(-1, 0, 0, IORING_ENTER_GETEVENTS, nullptr, 0); }"
        FAIO_LIBURING_NATIVE_API)
    cmake_pop_check_state()

    # 查到文件但 API 不兼容时，明确给出升级或关闭原生后端的处理方式。
    if(NOT FAIO_LIBURING_NATIVE_API)
        message(FATAL_ERROR "faio native io_uring requires liburing headers/library with FTRUNCATE, IORING_ACCEPT_DONTWAIT and io_uring_enter2; upgrade liburing (validated with 2.14), or configure FAIO_ENABLE_IO_URING=OFF")
    endif()
endif()

# 标准模块统一处理 REQUIRED/QUIET 与版本要求，并设置 Liburing_FOUND。
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Liburing
    REQUIRED_VARS Liburing_INCLUDE_DIR Liburing_LIBRARY
    VERSION_VAR Liburing_VERSION)

# 不重复定义目标；UNKNOWN 允许找到的静态或动态库均由实际路径决定类型。
if(Liburing_FOUND AND NOT TARGET Liburing::Liburing)
    add_library(Liburing::Liburing UNKNOWN IMPORTED)

    # 消费方只链接目标即可得到库路径和头文件搜索路径。
    set_target_properties(Liburing::Liburing PROPERTIES
        IMPORTED_LOCATION "${Liburing_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${Liburing_INCLUDE_DIR}")
endif()

# 文件路径属于高级配置项，普通 GUI 配置列表无需展示这些底层缓存变量。
mark_as_advanced(Liburing_INCLUDE_DIR Liburing_LIBRARY)
