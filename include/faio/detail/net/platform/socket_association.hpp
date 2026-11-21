/**
 * @file faio/detail/net/platform/socket_association.hpp
 * @brief socket 所属后端关联。
 */
#pragma once
#if defined(_WIN32)
#include "faio/detail/net/platform/windows/socket_association.hpp"
#else
#include "faio/detail/net/platform/unix/socket_association.hpp"
#endif
