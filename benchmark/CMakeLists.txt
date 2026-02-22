add_executable(coroutine_stress coroutine_stress.cpp)
target_include_directories(coroutine_stress PUBLIC ../include ../thirdparty)
target_link_libraries(coroutine_stress ${LIBS})


find_package(Boost REQUIRED COMPONENTS system)
find_package(asio CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_executable(faio_http_benchmark http/faio_http_benchmark.cpp)
target_include_directories(faio_http_benchmark PUBLIC ../include ../thirdparty)
target_link_libraries(faio_http_benchmark ${LIBS})

add_executable(beast_http_benchmark http/beast_http_benchmark.cpp)
target_link_libraries(beast_http_benchmark Boost::system
    Threads::Threads)

add_executable(faio_tcp_benmark tcp/faio_tcp_benmark.cpp)
target_include_directories(faio_tcp_benmark PUBLIC ../include ../thirdparty)
target_link_libraries(faio_tcp_benmark ${LIBS})

add_executable(asio_tcp_benchmark tcp/asio_tcp_benchmark.cpp)
target_link_libraries(asio_tcp_benchmark asio::asio Threads::Threads)


