include_guard(GLOBAL)

# Function scope deliberately leaves the base project's C++23 settings intact.
function(faio_check_experimental_toolchain reflection_enabled)
    if(CMAKE_VERSION VERSION_LESS "4.1")
        message(FATAL_ERROR "faio experimental requires CMake >= 4.1")
    endif()
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux"
       OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
       OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS "16.1"
       OR NOT CMAKE_CXX_COMPILER_VERSION VERSION_LESS "16.2")
        message(FATAL_ERROR "faio experimental requires Linux + GCC 16.1.x + matching libstdc++ 16.1")
    endif()
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -dumpfullversion -dumpversion
        OUTPUT_VARIABLE _faio_version OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -dumpmachine
        OUTPUT_VARIABLE _faio_machine OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=libstdc++.so
        OUTPUT_VARIABLE _faio_library OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    message(STATUS "faio experimental compiler: ${CMAKE_CXX_COMPILER} ${_faio_version} ${_faio_machine}")
    message(STATUS "faio experimental libstdc++: ${_faio_library}")
    message(STATUS "faio experimental compiler include paths: ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES}")

    set(CMAKE_CXX_STANDARD 26)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    include(CheckCXXSourceCompiles)
    check_cxx_source_compiles([=[
        #include <bits/c++config.h>
        #include <version>
        #if !defined(__linux__) || defined(__clang__) || __GNUC__ != 16 || __GNUC_MINOR__ != 1
        #error faio experimental: compiler/platform mismatch
        #endif
        #if !defined(__GLIBCXX__) || _GLIBCXX_RELEASE != 16
        #error faio experimental: matching libstdc++ 16 is required
        #endif
        static_assert(__cplusplus > 202302L);
        int main() {}
    ]=] FAIO_EXPERIMENTAL_COMPILER_LIBRARY_MATCH)
    if(NOT FAIO_EXPERIMENTAL_COMPILER_LIBRARY_MATCH)
        message(FATAL_ERROR "faio experimental: compiler, language mode, or libstdc++ identity probe failed")
    endif()
    if(reflection_enabled)
        set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -freflection")
        check_cxx_source_compiles([=[
            #include <meta>
            struct options { int value; };
            [[=options{42}]] int annotated() { return 42; }
            constexpr auto value = std::meta::extract<options>(
                std::meta::annotations_of_with_type(^^annotated, ^^options)[0]);
            static_assert(value.value == 42);
            int main() { return [: ^^annotated :]() == 42 ? 0 : 1; }
        ]=] FAIO_EXPERIMENTAL_REFLECTION_COMPILES)
        if(NOT FAIO_EXPERIMENTAL_REFLECTION_COMPILES)
            message(FATAL_ERROR "faio experimental: <meta>, annotations, extract, or splice probe failed with -freflection")
        endif()
    endif()
endfunction()

# Validate the fixed backend independently of the optional reflection component.
# This also runs for an installed consumer using only its vendored include tree.
function(faio_check_experimental_execution_contract header_directory)
    set(CMAKE_CXX_STANDARD 26)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
    list(APPEND CMAKE_REQUIRED_INCLUDES "${header_directory}")
    list(APPEND CMAKE_REQUIRED_LIBRARIES Threads::Threads)
    include(CheckCXXSourceCompiles)
    check_cxx_source_compiles([=[
        #include <stdexec/execution.hpp>
        #include <exec/task.hpp>
        #include <concepts>
        #include <stop_token>
        #include <tuple>

        static_assert(stdexec::stoppable_token<std::stop_token>);
        static_assert(stdexec::sender<decltype(stdexec::just(42))>);
        static_assert(stdexec::sender<exec::task<int>>);
        static_assert(std::same_as<
            stdexec::completion_signatures_of_t<decltype(stdexec::just(42)), stdexec::env<>>,
            stdexec::completion_signatures<stdexec::set_value_t(int)>>);

        exec::task<int> backend_task() {
            co_return co_await stdexec::just(42);
        }
        int main() {
            const auto value = stdexec::sync_wait(backend_task());
            return value && std::get<0>(*value) == 42 ? 0 : 1;
        }
    ]=] FAIO_EXPERIMENTAL_EXECUTION_CONTRACT_COMPILES)
    if(NOT FAIO_EXPERIMENTAL_EXECUTION_CONTRACT_COMPILES)
        message(FATAL_ERROR "faio experimental: fixed stdexec sender/task concept contract probe failed")
    endif()
    message(STATUS "faio experimental stdexec concept probe: ${header_directory}")
endfunction()
