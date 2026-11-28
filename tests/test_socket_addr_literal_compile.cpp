/**
 * @file test_socket_addr_literal_compile.cpp
 * @brief 字面量端点的编译成功与编译失败用例。
 */
#include "faio/net/address.hpp"

/** @brief 通过独立构建目标验证合法字面量可用、非法字面量无法通过编译。 */
void check_socket_addr_literal() {
#if FAIO_SOCKET_ADDR_LITERAL_CASE == 0
  (void)faio::net::socket_addr::from_string("127.0.0.1:8080");
  (void)faio::net::socket_addr::from_string("0.0.0.0:0");
  (void)faio::net::socket_addr::from_string("255.255.255.255:65535");
  (void)faio::net::socket_addr::from_string("[::]:0");
  (void)faio::net::socket_addr::from_string("[::1]:8080");
  (void)faio::net::socket_addr::from_string("[1:2:3:4:5:6:7:8]:65535");
  (void)faio::net::socket_addr::from_string("[2001:db8::]:80");
  (void)faio::net::socket_addr::from_string("[::ffff:192.0.2.1]:80");
  (void)faio::net::socket_addr::from_string("[1:2:3:4:5:6:192.0.2.1]:80");
  (void)faio::net::socket_addr::from_string("[fe80::1%3]:80");
#elif FAIO_SOCKET_ADDR_LITERAL_CASE == 1
  (void)faio::net::socket_addr::from_string("256.0.0.1:80");
#elif FAIO_SOCKET_ADDR_LITERAL_CASE == 2
  (void)faio::net::socket_addr::from_string("[1::2::3]:80");
#elif FAIO_SOCKET_ADDR_LITERAL_CASE == 3
  (void)faio::net::socket_addr::from_string("127.0.0.1:65536");
#elif FAIO_SOCKET_ADDR_LITERAL_CASE == 4
  (void)faio::net::socket_addr::from_string("127.0.0.1:80\0ignored");
#else
#error Unknown socket address literal test case
#endif
}
