#[[
@file cmake/FaioProcess.cmake
@brief 异步进程模块的依赖与平台配置。
]]
option(FAIO_ENABLE_PROCESS "Build the optional asynchronous process component" OFF)
if(NOT FAIO_ENABLE_PROCESS)
    return()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/FaioProcessPlatform.cmake")
faio_check_process_platform()
add_library(faio_process INTERFACE)
add_library(faio::process ALIAS faio_process)
set_target_properties(faio_process PROPERTIES EXPORT_NAME process)
target_link_libraries(faio_process INTERFACE faio::faio Threads::Threads)
target_compile_definitions(faio_process INTERFACE FAIO_ENABLE_PROCESS=1)
if(WIN32)
    target_link_libraries(faio_process INTERFACE bcrypt advapi32)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(faio_process INTERFACE _GNU_SOURCE)
endif()
if(FAIO_INSTALL)
    install(TARGETS faio_process EXPORT faioProcessTargets)
    install(EXPORT faioProcessTargets FILE faioProcessTargets.cmake
        NAMESPACE faio:: DESTINATION "${CMAKE_INSTALL_DATADIR}/cmake/faio")
endif()
