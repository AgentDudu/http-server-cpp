#include "net/detail_raw.h"

#include <windows.h>

#include <climits>

namespace net {
namespace detail {

handle_t rawSocket() {
    return ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
}

// No socketpair() on Winsock: build the equivalent from a loopback listener.
// Setup runs blocking (one-time); the pair leaves non-blocking.
bool rawSocketPair(handle_t out[2]) {
    const handle_t l = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (l == kInvalidHandle) {
        return false;
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    int len = sizeof(a);
    bool ok = ::bind(l, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != SOCKET_ERROR &&
              ::listen(l, 1) != SOCKET_ERROR &&
              ::getsockname(l, reinterpret_cast<sockaddr*>(&a), &len) != SOCKET_ERROR;
    const handle_t c = ok ? ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) : kInvalidHandle;
    if (ok && c != kInvalidHandle) {
        ok = ::connect(c, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != SOCKET_ERROR;
    }
    if (ok) {
        out[0] = ::accept(l, nullptr, nullptr);
        out[1] = c;
        ok = out[0] != kInvalidHandle;
    }
    ::closesocket(l);
    if (!ok) {
        if (c != kInvalidHandle) {
            ::closesocket(c);
        }
        return false;
    }
    rawSetNonBlocking(out[0], true);
    rawSetNonBlocking(out[1], true);
    return true;
}

int rawClose(handle_t handle) {
    return ::closesocket(handle);
}

bool rawSetNonBlocking(handle_t handle, bool enabled) {
    u_long mode = enabled ? 1UL : 0UL;
    return ::ioctlsocket(handle, FIONBIO, &mode) != SOCKET_ERROR;
}

handle_t rawAccept(handle_t listener) {
    return ::accept(listener, nullptr, nullptr);
}

// Winsock: the accepted socket already inherited the listener's blocking mode.
void rawAfterAccept(handle_t, handle_t) {}

int rawSend(handle_t handle, const void* data, std::size_t size) {
    return ::send(handle, static_cast<const char*>(data), static_cast<int>(size), 0);
}

int rawRecv(handle_t handle, void* buffer, std::size_t capacity) {
    return ::recv(handle, static_cast<char*>(buffer), static_cast<int>(capacity), 0);
}

int rawPoll(pollfd_t* fds, unsigned long count, int timeoutMs) {
    return ::WSAPoll(fds, static_cast<ULONG>(count), timeoutMs);
}

} // namespace detail
} // namespace net
