#pragma once

#include "net/detail.h"

#include <cstddef>

namespace net {
namespace detail {

// OS primitives. Error detail stays in the OS error slot (WSAGetLastError/errno);
// read it via net::lastError() immediately after a failed call.

handle_t rawSocket();                     // stream socket, IPv4 (CloExec where supported)
int rawClose(handle_t);                   // 0 on success
bool rawSetNonBlocking(handle_t, bool);
handle_t rawAccept(handle_t listener);    // raw; then call rawAfterAccept
// Make the accepted socket's blocking mode match the listener's, so the portable
// contract holds on both OSes: Windows inherits automatically, Linux does not.
void rawAfterAccept(handle_t listener, handle_t accepted);
// size must be <= INT_MAX (the shared layer chunks). >0 sent, -1 error.
int rawSend(handle_t, const void* data, std::size_t size); // never fails on EINTR
// >0 read, 0 EOF, -1 error (EINTR retried internally)
int rawRecv(handle_t, void* buffer, std::size_t capacity);
// >0 ready, 0 timeout, -1 error. EINTR is reported (ErrorKind::Interrupted).
int rawPoll(pollfd_t* fds, unsigned long count, int timeoutMs);

} // namespace detail
} // namespace net
