add_executable(tcp_server tcp_server.cpp)
target_link_libraries(tcp_server PRIVATE faio::faio)

add_executable(coroutine_task coroutine_task.cpp)
target_link_libraries(coroutine_task PRIVATE faio::faio)

add_executable(udp_server udp_server.cpp)
target_link_libraries(udp_server PRIVATE faio::faio)


add_executable(sync_demo sync_demo.cpp)
target_link_libraries(sync_demo PRIVATE faio::faio)
