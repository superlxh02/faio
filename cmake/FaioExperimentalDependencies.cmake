# Only inspect a previously bootstrapped checkout. CMake configure never fetches
# dependencies or enters upstream stdexec's network-enabled CMake project.
include_guard(GLOBAL)

function(faio_find_stdexec)
    if(TARGET faio_stdexec)
        return()
    endif()

    get_filename_component(_faio_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    if(DEFINED FAIO_DEPENDENCY_ROOT)
        set(_faio_deps "${FAIO_DEPENDENCY_ROOT}")
    else()
        get_filename_component(_faio_deps "${_faio_root}/../faio-deps" ABSOLUTE)
    endif()
    set(_faio_stdexec_default "${_faio_deps}/src/stdexec")
    if(DEFINED ENV{FAIO_STDEXEC_SOURCE_DIR} AND NOT "$ENV{FAIO_STDEXEC_SOURCE_DIR}" STREQUAL "")
        set(_faio_stdexec_default "$ENV{FAIO_STDEXEC_SOURCE_DIR}")
    endif()
    set(FAIO_STDEXEC_SOURCE_DIR "${_faio_stdexec_default}" CACHE PATH
        "Clean Git checkout of the locked experimental stdexec dependency")

    file(READ "${_faio_root}/scripts/experimental_dependencies.lock.json" _faio_lock)
    string(JSON _faio_commit GET "${_faio_lock}" stdexec commit)
    string(JSON _faio_repository GET "${_faio_lock}" stdexec repository)
    string(JSON _faio_tag GET "${_faio_lock}" stdexec tag)
    if(NOT EXISTS "${FAIO_STDEXEC_SOURCE_DIR}/.git"
       OR NOT EXISTS "${FAIO_STDEXEC_SOURCE_DIR}/include/stdexec/execution.hpp"
       OR NOT EXISTS "${FAIO_STDEXEC_SOURCE_DIR}/include/exec/task.hpp")
        message(FATAL_ERROR
            "Missing locked stdexec checkout. Run: python3 ${_faio_root}/scripts/bootstrap_experimental_deps.py --workspace-root ${_faio_deps}")
    endif()

    find_package(Git REQUIRED)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${FAIO_STDEXEC_SOURCE_DIR}" rev-parse HEAD
        RESULT_VARIABLE _faio_git_result OUTPUT_VARIABLE _faio_head
        ERROR_VARIABLE _faio_git_error OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _faio_git_result EQUAL 0 OR NOT _faio_head STREQUAL _faio_commit)
        message(FATAL_ERROR "stdexec HEAD must be ${_faio_commit}; found '${_faio_head}'. ${_faio_git_error}")
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${FAIO_STDEXEC_SOURCE_DIR}"
        status --porcelain --untracked-files=all
        RESULT_VARIABLE _faio_git_result OUTPUT_VARIABLE _faio_status
        ERROR_VARIABLE _faio_git_error OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _faio_git_result EQUAL 0 OR NOT _faio_status STREQUAL "")
        message(FATAL_ERROR "stdexec checkout must be clean: ${_faio_status} ${_faio_git_error}")
    endif()

    find_package(Threads REQUIRED)
    if(DEFINED CMAKE_INSTALL_INCLUDEDIR)
        set(_faio_install_include "${CMAKE_INSTALL_INCLUDEDIR}")
    else()
        set(_faio_install_include include)
    endif()
    add_library(faio_stdexec INTERFACE)
    target_include_directories(faio_stdexec SYSTEM INTERFACE
        "$<BUILD_INTERFACE:${FAIO_STDEXEC_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${_faio_install_include}/faio-third-party/stdexec>")
    target_link_libraries(faio_stdexec INTERFACE Threads::Threads)
    message(STATUS "faio stdexec: ${_faio_repository} ${_faio_tag} ${_faio_head} (local, clean)")
endfunction()
