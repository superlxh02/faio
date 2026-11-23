#[[
@file cmake/FaioImportStd.cmake
@brief 为模块示例开启 GCC 16.1 及以上的 C++23 标准库模块（`import std;`）。

CMake 把 `import std;` 视为实验特性：需要与被调用的 CMake 版本完全一致的
CMAKE_EXPERIMENTAL_CXX_IMPORT_STD 值，并且必须在 project() / enable_language(CXX)
之前设置。本文件按 CMake 版本给出该值，并提供只作用于模块示例的
faio_import_std_supported() 与 faio_enable_import_std()；其他目标仍然使用
标准库头文件。
]]

# 根工程与独立示例工程都会 include 本文件；保持幂等，用户预置的值优先。
if(NOT DEFINED CMAKE_EXPERIMENTAL_CXX_IMPORT_STD)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.30 AND CMAKE_VERSION VERSION_LESS 4.0)
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "0e5b6991-d74f-4b3d-a41c-cf096e0b2508")
    elseif(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0 AND CMAKE_VERSION VERSION_LESS 4.3)
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
    elseif(CMAKE_VERSION VERSION_GREATER_EQUAL 4.3 AND CMAKE_VERSION VERSION_LESS 4.4)
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
    elseif(CMAKE_VERSION VERSION_GREATER_EQUAL 4.4 AND CMAKE_VERSION VERSION_LESS 4.5)
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "f35a9ac6-8463-4d38-8eec-5d6008153e7d")
    endif()
endif()

#[[
@brief 判断当前工具链能否编译 `import std;`。
@param out_var 输出变量名，结果为 TRUE 或 FALSE。
]]
function(faio_import_std_supported out_var)
    set(supported FALSE)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
            AND NOT CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16.1
            AND CMAKE_CXX_COMPILER_IMPORT_STD)
        set(supported TRUE)
    endif()
    set(${out_var} ${supported} PARENT_SCOPE)
endfunction()

#[[
@brief 在 GCC 16.1 及以上把标准库模块接入单个示例目标。
@param target 已经创建好的可执行目标。
]]
function(faio_enable_import_std target)
    set(supported FALSE)
    faio_import_std_supported(supported)
    if(NOT supported)
        message(FATAL_ERROR
            "faio: ${target} 默认 import std;，需要 GCC 16.1 及以上（当前 "
            "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}）。")
    endif()
    set_target_properties(${target} PROPERTIES CXX_MODULE_STD ON)
    message(STATUS "faio: ${target} imports the C++23 std module (GCC ${CMAKE_CXX_COMPILER_VERSION})")
endfunction()
