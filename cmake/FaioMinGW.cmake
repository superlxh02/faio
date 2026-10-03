# Windows GCC 的工具链兼容模块，同时供源码构建和安装包消费使用。
#
# MSVC、clang-cl 与非 Windows 平台的入口直接返回，不改动其编译器设置。
# MinGW 路径先验证原生 TLS，再配置对象级精确修复；不修改协程或 runtime 源码，
# 不放宽链接器的重复定义诊断，也不把所有翻译单元合成 unity 构建。
# 所需工具只有 Python 解释器以及 MinGW 的 nm/objcopy，实际对象处理在同目录
# faio_mingw_compile.py 中完成；源码安装时必须同时保留该辅助脚本。

# 安装包可能由多个子目录重复查找，函数定义只在当前配置进程中加载一次。
include_guard(GLOBAL)

# 验证实际代码生成采用 native TLS，而非只根据编译器版本号猜测。
# 已验证的旧 emutls 发行版存在 TLS 存储先于 CRT 对象析构释放的问题；
# 初始化 alias 的对象修复无法解决这一运行库生命周期缺陷，必须单独拒绝。
function(_faio_require_native_mingw_tls)
    # 探测文件仅写入构建目录，支持源码和安装包消费时各自独立配置。
    set(_probe_root "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles")
    file(MAKE_DIRECTORY "${_probe_root}")
    set(_source "${_probe_root}/faio-mingw-tls-probe.cpp")
    set(_assembly "${_probe_root}/faio-mingw-tls-probe.s")

    # 必须实际取得 TLS 变量地址，确保汇编包含该工具链的 TLS 访问方式。
    file(WRITE "${_source}" "thread_local int faio_tls_probe;\nint* faio_tls_address() { return &faio_tls_probe; }\n")

    # 保留用户的全局 C++ 编译选项，并按当前主机命令行规则拆分参数。
    separate_arguments(_flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS}")

    # -S 只生成汇编，不执行探测程序；-O0 避免探测访问被优化掉。
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" ${_flags} -std=c++23 -S -O0 "${_source}" -o "${_assembly}"
        RESULT_VARIABLE _result
        ERROR_VARIABLE _diagnostic
        OUTPUT_QUIET)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "faio could not verify MinGW native TLS: ${_diagnostic}")
    endif()

    # emutls 的访问会生成 __emutls_ 符号，必须在配置阶段明确诊断。
    file(READ "${_assembly}" _generated)
    if(_generated MATCHES "__emutls_")
        message(FATAL_ERROR "faio requires MinGW GCC native TLS (MSYS2 UCRT64 GCC 16+). This compiler uses emutls with unsafe CRT destructor ordering; use scripts/bootstrap_windows.ps1 and the windows-mingw preset.")
    endif()
endfunction()

# 生成编译 launcher 命令列表，保留已有 launcher 的全部参数与原有顺序。
# output：返回命令列表的父作用域变量名；existing：目标或目录已有的 launcher。
# 精确修复只处理 current_stop_token 的已知初始化 alias，不合并初始化函数体。
function(_faio_make_mingw_launcher output existing)
    # 使用 CMake 找到的解释器，路径作为独立列表项处理，允许安装目录含空格。
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    # 优先从当前编译器目录寻找工具，使对象处理工具与编译器发行版匹配。
    # find_program 保留标准缓存覆盖方式，工具缺失时在配置阶段直接失败。
    get_filename_component(_compiler_directory "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(FAIO_MINGW_OBJCOPY NAMES objcopy.exe objcopy HINTS "${_compiler_directory}" REQUIRED)
    find_program(FAIO_MINGW_NM NAMES nm.exe nm HINTS "${_compiler_directory}" REQUIRED)

    # 以函数定义所在目录定位脚本，保证安装后的配置不依赖源码工作目录。
    set(_script "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/faio_mingw_compile.py")

    # 目录默认设置与目标设置可能先后应用，已包含本工具时不重复套娃。
    if("${existing}" MATCHES "faio_mingw_compile[.]py")
        set(${output} "${existing}" PARENT_SCOPE)
        return()
    endif()

    # -- 后传递原编译命令；Python 脚本先执行该命令，再检查输出 COFF 对象。
    set(_launcher "${Python3_EXECUTABLE}" "${_script}" --objcopy "${FAIO_MINGW_OBJCOPY}" --nm "${FAIO_MINGW_NM}" --)
    if(existing)
        # ccache/sccache 等已有工具继续执行，并明确告知用户发生了串联。
        message(STATUS "faio: chaining the MinGW TLS workaround around existing CXX compiler launcher: ${existing}")
        list(APPEND _launcher ${existing})
    endif()

    # 只返回到调用方作用域，不强制改写用户的全局 CACHE 条目。
    set(${output} "${_launcher}" PARENT_SCOPE)
endfunction()

# 为已经创建的单个目标配置修复，也供安装包消费方显式调用。
# 在其他目录中早于 find_package(faio) 创建的目标，需由消费方调用此函数。
# target：现有 CMake 目标名；保留其专用 launcher，缺省时继承目录的 launcher。
function(faio_enable_mingw_tls_workaround_for_target target)
    # GNU 在 Windows 上对应本模块支持的 MinGW；其他组合保持原设置。
    if(NOT WIN32 OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        return()
    endif()

    # 拼错目标名是配置错误，不能静默漏掉需要修复的编译对象。
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "faio MinGW TLS workaround target does not exist: ${target}")
    endif()

    # 获取目标的已有设置；未设置属性时使用目录级默认值作为串联基础。
    get_target_property(_existing "${target}" CXX_COMPILER_LAUNCHER)
    if(_existing STREQUAL "_existing-NOTFOUND")
        set(_existing "${CMAKE_CXX_COMPILER_LAUNCHER}")
    endif()

    _faio_make_mingw_launcher(_launcher "${_existing}")
    set_property(TARGET "${target}" PROPERTY CXX_COMPILER_LAUNCHER "${_launcher}")
endfunction()

# 启用调用目录的 MinGW 修复，覆盖后续新建目标及当前目录内已有的实体目标。
# 此入口由根项目和已安装的 faioConfig.cmake 调用，二者采用相同的工具链约束。
function(faio_enable_mingw_tls_workaround)
    if(NOT WIN32 OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        return()
    endif()

    # 先确认运行库所需的 TLS 访问机制，再将精确对象处理加入编译命令。
    _faio_require_native_mingw_tls()
    _faio_make_mingw_launcher(_launcher "${CMAKE_CXX_COMPILER_LAUNCHER}")

    # 后续 add_library/add_executable 从调用方目录变量继承默认 launcher。
    set(CMAKE_CXX_COMPILER_LAUNCHER "${_launcher}" PARENT_SCOPE)

    # 只遍历本目录目标；不跨目录擅自改写其他项目已经配置的目标。
    get_property(_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_target IN LISTS _targets)
        get_target_property(_type "${_target}" TYPE)

        # INTERFACE_LIBRARY 和 UTILITY 没有 C++ 对象编译步骤，无需配置 launcher。
        if(NOT _type STREQUAL "INTERFACE_LIBRARY" AND NOT _type STREQUAL "UTILITY")
            faio_enable_mingw_tls_workaround_for_target("${_target}")
        endif()
    endforeach()

    # 显式报告启用状态，方便用户审阅编译链和排查安装消费问题。
    message(STATUS "faio: enabled precise MinGW GCC inline TLS alias workaround")
endfunction()
