add_executable(tcp_server tcp_server.cpp)
target_include_directories(tcp_server PUBLIC ../include)
target_link_libraries(tcp_server PUBLIC uring)

add_executable(coroutine_task coroutine_task.cpp)
target_include_directories(coroutine_task PUBLIC ../include)
target_link_libraries(coroutine_task PUBLIC uring)

add_executable(udp_server udp_server.cpp)
target_include_directories(udp_server PUBLIC ../include)
target_link_libraries(udp_server PUBLIC uring)


add_executable(sync_demo sync_demo.cpp)
target_include_directories(sync_demo PUBLIC ../include)
target_link_libraries(sync_demo PUBLIC uring)




