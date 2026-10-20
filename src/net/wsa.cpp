#include "net/socket.h"

namespace net {

WsaContext::WsaContext() {
    const int rc = WSAStartup(MAKEWORD(2, 2), &wsaData_);
    ok_ = (rc == 0);
    version_ = ok_ ? wsaData_.wVersion : 0;
}

WsaContext::~WsaContext() {
    if (ok_) {
        WSACleanup();
    }
}

} // namespace net
