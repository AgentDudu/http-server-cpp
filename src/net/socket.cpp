#include "net/socket.h"

#include <windows.h> // Sleep, used by the blocking test helper only

#include <climits>

namespace net {

Socket Socket::tcp4() {
    return Socket(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
}

bool Socket::setNonBlocking(bool enabled) {
    if (!valid()) {
        WSASetLastError(WSAENOTSOCK);
        return false;
    }
    u_long mode = enabled ? 1UL : 0UL;
    return ::ioctlsocket(handle_, FIONBIO, &mode) != SOCKET_ERROR;
}

bool Socket::setReuseAddress() {
    if (!valid()) {
        WSASetLastError(WSAENOTSOCK);
        return false;
    }
    char on = 1;
    return ::setsockopt(handle_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != SOCKET_ERROR;
}

bool Socket::bindAny(unsigned short port) {
    sockaddr_in name{};
    name.sin_family = AF_INET;
    name.sin_addr.s_addr = INADDR_ANY;
    name.sin_port = htons(port);
    return ::bind(handle_, reinterpret_cast<const sockaddr*>(&name), sizeof(name)) !=
           SOCKET_ERROR;
}

unsigned short Socket::localPort() const {
    sockaddr_in name{};
    int len = static_cast<int>(sizeof(name));
    if (::getsockname(handle_, reinterpret_cast<sockaddr*>(&name), &len) == SOCKET_ERROR) {
        return 0;
    }
    return ntohs(name.sin_port);
}

bool Socket::listen(int backlog) {
    return valid() && ::listen(handle_, backlog) != SOCKET_ERROR;
}

Socket Socket::accept() {
    if (!valid()) {
        WSASetLastError(WSAENOTSOCK);
        return Socket();
    }
    // No accepted-socket address requested; the reactor learns peers only if needed.
    return Socket(::accept(handle_, nullptr, nullptr));
}

bool Socket::connect(const sockaddr_in& address) {
    if (!valid()) {
        WSASetLastError(WSAENOTSOCK);
        return false;
    }
    return ::connect(handle_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) !=
           SOCKET_ERROR;
}

bool Socket::close() {
    if (valid()) {
        const SOCKET dead = handle_;
        handle_ = INVALID_SOCKET;
        return ::closesocket(dead) == 0;
    }
    return true;
}

bool Socket::sendAll(const char* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        const int chunk = static_cast<int>(len - sent > static_cast<std::size_t>(INT_MAX)
                                               ? INT_MAX
                                               : len - sent);
        const int n = ::send(handle_, data + sent, chunk, 0);
        if (n == SOCKET_ERROR) {
            const int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                Sleep(1); // shortcut: blocking-test helper only; the reactor polls POLLOUT
                continue;
            }
            WSASetLastError(err);
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

int Socket::recvSome(void* buffer, std::size_t capacity) {
    const int chunk = capacity > static_cast<std::size_t>(INT_MAX)
                          ? INT_MAX
                          : static_cast<int>(capacity);
    const int n = ::recv(handle_, static_cast<char*>(buffer), chunk, 0);
    return n == SOCKET_ERROR ? -1 : n;
}

} // namespace net
