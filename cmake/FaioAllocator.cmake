#[[
@file cmake/FaioAllocator.cmake
@brief 配置 Linux 进程分配器及安装包提供的目标。
]]
include_guard(GLOBAL)

#[[
@brief 根据平台、Sanitizer 配置和可用依赖，为目标选择动态内存分配器。
]]
function(faio_configure_allocator)
    if(TARGET faio::allocator)
        get_target_property(_faio_effective faio::allocator FAIO_ALLOCATOR_EFFECTIVE)
        set(FAIO_ALLOCATOR_EFFECTIVE "${_faio_effective}" PARENT_SCOPE)
        return()
    endif()
    add_library(faio::allocator INTERFACE IMPORTED GLOBAL)
    set(FAIO_ALLOCATOR "AUTO" CACHE STRING "AUTO, SYSTEM or TCMALLOC")
    set_property(CACHE FAIO_ALLOCATOR PROPERTY STRINGS AUTO SYSTEM TCMALLOC)
    string(TOUPPER "${FAIO_ALLOCATOR}" _faio_allocator)
    if(NOT _faio_allocator MATCHES "^(AUTO|SYSTEM|TCMALLOC)$")
        message(FATAL_ERROR "FAIO_ALLOCATOR must be AUTO, SYSTEM or TCMALLOC")
    endif()
    set(_faio_sanitizer_flags "${CMAKE_CXX_FLAGS} ${CMAKE_EXE_LINKER_FLAGS}")
    set(_faio_configurations "${CMAKE_BUILD_TYPE};${CMAKE_CONFIGURATION_TYPES}")
    foreach(_faio_configuration IN LISTS _faio_configurations)
        string(TOUPPER "${_faio_configuration}" _faio_configuration)
        string(APPEND _faio_sanitizer_flags
            " ${CMAKE_CXX_FLAGS_${_faio_configuration}} ${CMAKE_EXE_LINKER_FLAGS_${_faio_configuration}}")
    endforeach()
    set(_faio_sanitized FALSE)
    if(FAIO_SANITIZERS OR _faio_sanitizer_flags MATCHES "-fsanitize[= ]")
        set(_faio_sanitized TRUE)
    endif()
    if(_faio_allocator STREQUAL "TCMALLOC" AND
       (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR _faio_sanitized))
        message(FATAL_ERROR "FAIO_ALLOCATOR=TCMALLOC requires Linux without sanitizers; use SYSTEM")
    endif()
    set(_faio_effective SYSTEM)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT _faio_sanitized AND
       NOT _faio_allocator STREQUAL "SYSTEM")
        list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
        if(_faio_allocator STREQUAL "TCMALLOC")
            find_package(TCMalloc REQUIRED)
        else()
            find_package(TCMalloc QUIET)
        endif()
        if(TCMalloc_FOUND)
            set(_faio_effective TCMALLOC)
            if(TCMalloc_LIBRARY MATCHES "\\.a$")
                set(_faio_link_state "-Wl,--push-state,--whole-archive")
            else()
                set(_faio_link_state "-Wl,--push-state,--no-as-needed")
            endif()
            target_link_libraries(faio::allocator INTERFACE
                "${_faio_link_state}" TCMalloc::TCMalloc "-Wl,--pop-state")
        endif()
    endif()
    set_target_properties(faio::allocator PROPERTIES FAIO_ALLOCATOR_EFFECTIVE "${_faio_effective}")
    set(FAIO_ALLOCATOR_EFFECTIVE "${_faio_effective}" PARENT_SCOPE)
    message(STATUS "faio allocator: ${_faio_effective} (requested ${FAIO_ALLOCATOR})")
endfunction()
