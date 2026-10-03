# Linux原生Proactor依赖。系统包与vcpkg都可以提供liburing.h及uring库。
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_FAIO_LIBURING QUIET liburing)
endif()
find_path(Liburing_INCLUDE_DIR liburing.h HINTS ${PC_FAIO_LIBURING_INCLUDE_DIRS})
find_library(Liburing_LIBRARY NAMES uring HINTS ${PC_FAIO_LIBURING_LIBRARY_DIRS})
set(Liburing_VERSION "${PC_FAIO_LIBURING_VERSION}")
# 在配置和安装消费阶段验证原生适配需要的头文件API，清楚诊断旧liburing。
# 内核opcode可用性仍由运行时probe判定，不能用用户态库版本冒充内核能力。
if(Liburing_INCLUDE_DIR AND Liburing_LIBRARY)
    include(CheckCXXSourceCompiles)
    include(CMakePushCheckState)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_INCLUDES "${Liburing_INCLUDE_DIR}")
    set(CMAKE_REQUIRED_LIBRARIES "${Liburing_LIBRARY}")
    check_cxx_source_compiles("#include <liburing.h>\nint main() { io_uring_sqe sqe{}; io_uring_prep_ftruncate(&sqe, -1, 0); io_uring_prep_accept(&sqe, -1, nullptr, nullptr, IORING_ACCEPT_DONTWAIT); return io_uring_enter2(-1, 0, 0, IORING_ENTER_GETEVENTS, nullptr, 0); }" FAIO_LIBURING_NATIVE_API)
    cmake_pop_check_state()
    if(NOT FAIO_LIBURING_NATIVE_API)
        message(FATAL_ERROR "faio native io_uring requires liburing headers/library with FTRUNCATE, IORING_ACCEPT_DONTWAIT and io_uring_enter2; upgrade liburing (validated with 2.14), or configure FAIO_ENABLE_IO_URING=OFF")
    endif()
endif()
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Liburing
    REQUIRED_VARS Liburing_INCLUDE_DIR Liburing_LIBRARY
    VERSION_VAR Liburing_VERSION)
if(Liburing_FOUND AND NOT TARGET Liburing::Liburing)
    add_library(Liburing::Liburing UNKNOWN IMPORTED)
    set_target_properties(Liburing::Liburing PROPERTIES
        IMPORTED_LOCATION "${Liburing_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${Liburing_INCLUDE_DIR}")
endif()
mark_as_advanced(Liburing_INCLUDE_DIR Liburing_LIBRARY)
