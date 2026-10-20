#pragma once

// Winsock 2 session owner. Exactly one per process, created before any Socket and
// destroyed after the last one is gone.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

namespace net {

class WsaContext {
  public:
    WsaContext();
    ~WsaContext();
    WsaContext(const WsaContext&) = delete;
    WsaContext& operator=(const WsaContext&) = delete;

    bool ok() const { return ok_; }
    int version() const { return version_; } // negotiated Winsock version, e.g. 0x0202

  private:
    WSADATA wsaData_;
    bool ok_ = false;
    int version_ = 0;
};

} // namespace net
