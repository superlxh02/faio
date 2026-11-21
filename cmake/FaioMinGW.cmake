#[[
@file cmake/FaioMinGW.cmake
@brief 配置 MinGW 原生 TLS 与编译启动器。
]]
include_guard(GLOBAL)

#[[ @brief 检查 MinGW 工具链是否支持原生 TLS。 ]]
function(_faio_require_native_mingw_tls)

    set(_probe_root "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles")
    file(MAKE_DIRECTORY "${_probe_root}")
    set(_source "${_probe_root}/faio-mingw-tls-probe.cpp")
    set(_assembly "${_probe_root}/faio-mingw-tls-probe.s")

    file(WRITE "${_source}" "thread_local int faio_tls_probe;\nint* faio_tls_address() { return &faio_tls_probe; }\n")

    separate_arguments(_flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS}")

    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" ${_flags} -std=c++23 -S -O0 "${_source}" -o "${_assembly}"
        RESULT_VARIABLE _result
        ERROR_VARIABLE _diagnostic
        OUTPUT_QUIET)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "faio could not verify MinGW native TLS: ${_diagnostic}")
    endif()

    file(READ "${_assembly}" _generated)
    if(_generated MATCHES "__emutls_")
        message(FATAL_ERROR "faio requires MinGW GCC native TLS (MSYS2 UCRT64 GCC 16+). This compiler uses emutls with unsafe CRT destructor ordering; use scripts/bootstrap_windows.ps1 and the windows-mingw preset.")
    endif()
endfunction()

#[[ @brief 将 MinGW 对象适配器与现有编译启动器组合使用。 ]]
function(_faio_make_mingw_launcher output existing)

    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    get_filename_component(_compiler_directory "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(FAIO_MINGW_OBJCOPY NAMES objcopy.exe objcopy HINTS "${_compiler_directory}" REQUIRED)
    find_program(FAIO_MINGW_NM NAMES nm.exe nm HINTS "${_compiler_directory}" REQUIRED)

    set(_script "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/faio_mingw_compile.py")

    if("${existing}" MATCHES "faio_mingw_compile[.]py")
        set(${output} "${existing}" PARENT_SCOPE)
        return()
    endif()

    set(_launcher "${Python3_EXECUTABLE}" "${_script}" --objcopy "${FAIO_MINGW_OBJCOPY}" --nm "${FAIO_MINGW_NM}" --)
    if(existing)

        message(STATUS "faio: chaining the MinGW TLS workaround around existing CXX compiler launcher: ${existing}")
        list(APPEND _launcher ${existing})
    endif()

    set(${output} "${_launcher}" PARENT_SCOPE)
endfunction()

#[[ @brief 为目标启用 MinGW 对象级 TLS 兼容适配。 ]]
function(faio_enable_mingw_tls_workaround_for_target target)

    if(NOT WIN32 OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        return()
    endif()

    if(NOT TARGET "${target}")
        message(FATAL_ERROR "faio MinGW TLS workaround target does not exist: ${target}")
    endif()

    get_target_property(_existing "${target}" CXX_COMPILER_LAUNCHER)
    if(_existing STREQUAL "_existing-NOTFOUND")
        set(_existing "${CMAKE_CXX_COMPILER_LAUNCHER}")
    endif()

    _faio_make_mingw_launcher(_launcher "${_existing}")
    set_property(TARGET "${target}" PROPERTY CXX_COMPILER_LAUNCHER "${_launcher}")
endfunction()

#[[ @brief 为构建目录启用 MinGW 原生 TLS 兼容适配。 ]]
function(faio_enable_mingw_tls_workaround)
    if(NOT WIN32 OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        return()
    endif()

    _faio_require_native_mingw_tls()
    _faio_make_mingw_launcher(_launcher "${CMAKE_CXX_COMPILER_LAUNCHER}")

    set(CMAKE_CXX_COMPILER_LAUNCHER "${_launcher}" PARENT_SCOPE)

    get_property(_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_target IN LISTS _targets)
        get_target_property(_type "${_target}" TYPE)

        if(NOT _type STREQUAL "INTERFACE_LIBRARY" AND NOT _type STREQUAL "UTILITY")
            faio_enable_mingw_tls_workaround_for_target("${_target}")
        endif()
    endforeach()

    message(STATUS "faio: enabled precise MinGW GCC inline TLS alias workaround")
endfunction()
