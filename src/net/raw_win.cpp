#include "net/detail_raw.h"

#include <windows.h>

#include <climits>

namespace net {
namespace detail {

handle_t rawSocket() {
    return ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
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
