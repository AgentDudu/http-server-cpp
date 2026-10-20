#pragma once

namespace net {

// OS error codes unified across Windows (WSAGetLastError) and POSIX (errno).
enum class ErrorKind {
    None,
    WouldBlock,  // non-blocking operation would block (WSAEWOULDBLOCK / EAGAIN)
    Interrupted, // call interrupted (poll EINTR); retry the wait
    Other
};

struct Error {
    ErrorKind kind = ErrorKind::None;
    int code = 0; // raw OS code, for logs and tests only
};

// Snapshot of the OS error slot; call immediately after the failed call.
Error lastError();

} // namespace net
