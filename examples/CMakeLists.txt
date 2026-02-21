add_executable(tcp_server tcp_server.cpp)
target_include_directories(tcp_server PUBLIC ../include ../thirdparty)
target_link_libraries(tcp_server ${LIBS})

add_executable(coroutine_task coroutine_task.cpp)
target_include_directories(coroutine_task PUBLIC ../include ../thirdparty)
target_link_libraries(coroutine_task  ${LIBS})

add_executable(udp_server udp_server.cpp)
target_include_directories(udp_server PUBLIC ../include ../thirdparty)
target_link_libraries(udp_server  ${LIBS} ) 


add_executable(sync_demo sync_demo.cpp)
target_include_directories(sync_demo PUBLIC ../include ../thirdparty)
target_link_libraries(sync_demo  ${LIBS} )



add_executable(http_server http_server.cpp)
target_include_directories(http_server PUBLIC ../include ../thirdparty)
target_link_libraries(http_server ${LIBS})

add_executable(http_client http_client.cpp)
target_include_directories(http_client PUBLIC ../include ../thirdparty)
target_link_libraries(http_client  ${LIBS})
