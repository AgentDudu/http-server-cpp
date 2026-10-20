#pragma once

// Platform seam. Nothing outside net/ includes OS socket headers.

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

namespace net {
namespace detail {

using handle_t = SOCKET;
inline constexpr handle_t kInvalidHandle = INVALID_SOCKET;
using pollfd_t = WSAPOLLFD;

} // namespace detail
} // namespace net

#else

// glibc hides accept4/SOCK_CLOEXEC/SOCK_NONBLOCK under -std=c++20 (strict ANSI);
// request GNU extensions explicitly.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>

namespace net {
namespace detail {

using handle_t = int;
inline constexpr handle_t kInvalidHandle = -1;
using pollfd_t = ::pollfd;

} // namespace detail
} // namespace net

#endif
