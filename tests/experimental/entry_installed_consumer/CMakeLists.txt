cmake_minimum_required(VERSION 4.1)
project(faio_entry_installed_consumer LANGUAGES CXX)
find_package(faio CONFIG REQUIRED COMPONENTS experimental_reflection)
add_executable(entry "../installed_consumer/async_main.cpp")
target_link_libraries(entry PRIVATE faio::experimental_reflection)
set_target_properties(entry PROPERTIES CXX_SCAN_FOR_MODULES OFF
    CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
