#include "net/context.h"

namespace net {

Context::Context() {
    const int rc = WSAStartup(MAKEWORD(2, 2), &wsaData_);
    ok_ = (rc == 0);
    version_ = ok_ ? wsaData_.wVersion : 0;
}

Context::~Context() {
    if (ok_) {
        WSACleanup();
    }
}

} // namespace net
