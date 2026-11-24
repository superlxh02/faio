#[[
@file cmake/FaioProtocols.cmake
@brief 配置可选的 TLS 与进程组件。
]]
option(FAIO_ENABLE_TLS "Build the optional OpenSSL TLS component" OFF)

if(NOT FAIO_ENABLE_TLS)
    return()
endif()

find_package(OpenSSL 3.5 REQUIRED COMPONENTS SSL Crypto)
target_link_libraries(faio INTERFACE OpenSSL::SSL OpenSSL::Crypto)
target_compile_definitions(faio INTERFACE FAIO_ENABLE_TLS=1)
add_library(faio_tls INTERFACE)
add_library(faio::tls ALIAS faio_tls)
set_target_properties(faio_tls PROPERTIES EXPORT_NAME tls)
target_link_libraries(faio_tls INTERFACE faio::faio)

if(FAIO_INSTALL)
    install(TARGETS faio_tls EXPORT faioTlsTargets)
    install(EXPORT faioTlsTargets FILE faioTlsTargets.cmake
        NAMESPACE faio:: DESTINATION "${CMAKE_INSTALL_DATADIR}/cmake/faio")
    configure_file("${CMAKE_CURRENT_LIST_DIR}/faioProtocolsConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/faioProtocolsConfig.cmake" @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/faioProtocolsConfig.cmake"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/cmake/faio")
endif()
