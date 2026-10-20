#include "net/error.h"

#include "net/detail.h"

namespace net {

Error lastError() {
    const int code = WSAGetLastError();
    switch (code) {
    case WSAEWOULDBLOCK:
        return {ErrorKind::WouldBlock, code};
    case WSAEINTR:
        return {ErrorKind::Interrupted, code};
    default:
        return {ErrorKind::Other, code};
    }
}

} // namespace net
