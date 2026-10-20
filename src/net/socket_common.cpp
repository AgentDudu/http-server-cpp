// Shared socket logic — identical on every platform. Only OS constants hide below.

#include "net/socket.h"

#include <algorithm>
#include <climits>

namespace net {

Socket Socket::tcp4() { return Socket(detail::rawSocket()); }

bool Socket::setNonBlocking(bool enabled) {
    return valid() && detail::rawSetNonBlocking(handle_, enabled);
}

bool Socket::setReuseAddress() {
    if (!valid()) {
        return false;
    }
    // int, not char: glibc's setsockopt enforces len == sizeof(int); Winsock takes either.
    int on = 1;
    return ::setsockopt(handle_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on),
                        sizeof(on)) != -1;
}

bool Socket::bindAny(unsigned short port) {
    if (!valid()) {
        return false;
    }
    sockaddr_in name{};
    name.sin_family = AF_INET;
    name.sin_addr.s_addr = INADDR_ANY;
    name.sin_port = htons(port);
    return ::bind(handle_, reinterpret_cast<const sockaddr*>(&name), sizeof(name)) != -1;
}

unsigned short Socket::localPort() const {
    if (!valid()) {
        return 0;
    }
    sockaddr_in name{};
    socklen_t len = sizeof(name);
    if (::getsockname(handle_, reinterpret_cast<sockaddr*>(&name), &len) == -1) {
        return 0;
    }
    return ntohs(name.sin_port);
}

bool Socket::listen(int backlog) {
    return valid() && ::listen(handle_, backlog) != -1;
}

Socket Socket::accept() {
    if (!valid()) {
        return Socket();
    }
    // Peer address not requested; the reactor learns peers only if needed.
    const detail::handle_t accepted = detail::rawAccept(handle_);
    if (accepted == detail::kInvalidHandle) {
        return Socket();
    }
    detail::rawAfterAccept(handle_, accepted);
    return Socket(accepted);
}

bool Socket::connect(const sockaddr_in& address) {
    if (!valid()) {
        return false;
    }
    return ::connect(handle_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) !=
           -1;
}

bool Socket::close() {
    if (valid()) {
        const detail::handle_t dead = handle_;
        handle_ = detail::kInvalidHandle;
        return detail::rawClose(dead) == 0;
    }
    return true;
}

bool Socket::sendAll(const char* data, std::size_t len) {
    if (!valid()) {
        return false;
    }
    std::size_t sent = 0;
    while (sent < len) {
        const std::size_t chunk = std::min(len - sent, static_cast<std::size_t>(INT_MAX));
        const int n = detail::rawSend(handle_, data + sent, chunk);
        if (n <= 0) {
            return false; // WouldBlock or error; lastError() tells which
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

int Socket::sendSome(const char* data, std::size_t len) {
    if (!valid() || len == 0) {
        return -1;
    }
    return detail::rawSend(handle_, data, std::min(len, static_cast<std::size_t>(INT_MAX)));
}

int Socket::recvSome(void* buffer, std::size_t capacity) {
    if (!valid()) {
        return -1;
    }
    return detail::rawRecv(
        handle_, buffer, std::min(capacity, static_cast<std::size_t>(INT_MAX)));
}

} // namespace net
