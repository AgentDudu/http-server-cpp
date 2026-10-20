#include "net/error.h"

#include "net/detail.h"

#include <cerrno>

namespace net {

Error lastError() {
    const int code = errno;
    switch (code) {
    case EAGAIN: // equals WSAEWOULDBLOCK's role; EWOULDBLOCK may equal EAGAIN
        return {ErrorKind::WouldBlock, code};
    case EINTR:
        return {ErrorKind::Interrupted, code};
    default:
        return {ErrorKind::Other, code};
    }
}

} // namespace net
