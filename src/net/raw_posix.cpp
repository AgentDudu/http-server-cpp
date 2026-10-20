#include "net/detail_raw.h"

#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <unistd.h>

namespace net {
namespace detail {

handle_t rawSocket() { return ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0); }

int rawClose(handle_t handle) { return ::close(handle); }

bool rawSetNonBlocking(handle_t handle, bool enabled) {
    const int flags = ::fcntl(handle, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    const int want = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (want == flags) {
        return true;
    }
    return ::fcntl(handle, F_SETFL, want) == 0;
}

handle_t rawAccept(handle_t listener) {
    // shortcut: accept4 exists since Linux 2.6.28; no pre-2008 fallback, CI runs 6.x.
    return ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
}

// Linux accepted sockets do NOT inherit the listener's O_NONBLOCK (opposite of
// Winsock). Copy the mode explicitly so the portable contract holds.
void rawAfterAccept(handle_t listener, handle_t accepted) {
    const int flags = ::fcntl(listener, F_GETFL, 0);
    if (flags >= 0 && (flags & O_NONBLOCK) != 0) {
        rawSetNonBlocking(accepted, true);
    }
}

int rawSend(handle_t handle, const void* data, std::size_t size) {
    int result;
    do {
        // MSG_NOSIGNAL: Linux raises SIGPIPE on send to a dead peer; we poll errors.
        const ssize_t n = ::send(handle, data, size, MSG_NOSIGNAL);
        if (n == -1) {
            result = -1;
        } else {
            return static_cast<int>(n);
        }
    } while (errno == EINTR);
    return result;
}

int rawRecv(handle_t handle, void* buffer, std::size_t capacity) {
    ssize_t n;
    do {
        n = ::recv(handle, buffer, capacity, 0);
    } while (n == -1 && errno == EINTR);
    return n == -1 ? -1 : static_cast<int>(n);
}

int rawPoll(pollfd_t* fds, unsigned long count, int timeoutMs) {
    // EINTR is reported, not retried: the reactor treats it as "loop again".
    return ::poll(fds, static_cast<nfds_t>(count), timeoutMs);
}

} // namespace detail
} // namespace net
