#pragma once

#include "net/detail.h"

namespace net {

// Process-wide socket subsystem session. One instance, created before any Socket,
// destroyed after the last Socket dies. No-op on POSIX.
class Context {
public:
    Context();
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    bool ok() const { return ok_; }
    int version() const { return version_; } // Winsock level on Windows, 0 on POSIX

private:
#ifdef _WIN32
    WSADATA wsaData_;
#endif
    bool ok_ = false;
    int version_ = 0;
};

} // namespace net
