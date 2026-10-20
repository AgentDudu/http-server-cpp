#pragma once

#include "net/wsa.h"

#include <cstddef>

namespace net {

// Move-only owning TCP socket handle. All calls return false / kInvalidSocket and
// leave the reason in WSAGetLastError(); call right after a failed call.
class Socket {
  public:
    Socket() = default;
    explicit Socket(SOCKET handle) : handle_(handle) {}
    ~Socket() { close(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : handle_(detach(other)) {}
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            close();
            handle_ = detach(other);
        }
        return *this;
    }

    static Socket tcp4(); // INVALID_SOCKET handle on failure

    SOCKET handle() const { return handle_; }
    bool valid() const { return handle_ != INVALID_SOCKET; }

    bool setNonBlocking(bool enabled);
    bool setReuseAddress();
    bool bindAny(unsigned short port); // INADDR_ANY; port 0 picks an ephemeral port
    unsigned short localPort() const;  // 0 on failure
    bool listen(int backlog);
    Socket accept();                   // INVALID_SOCKET handle when none pending
    bool connect(const sockaddr_in& address);
    bool close();

    // Blocking convenience for tests and diagnostics: loops until all bytes are
    // queued. The reactor never uses it; it buffers per-connection instead.
    bool sendAll(const char* data, std::size_t len);
    // >0 bytes read, 0 peer closed, -1 error (WSAEWOULDBLOCK possible on non-blocking).
    int recvSome(void* buffer, std::size_t capacity);

  private:
    static SOCKET detach(Socket& s) {
        SOCKET h = s.handle_;
        s.handle_ = INVALID_SOCKET;
        return h;
    }

    SOCKET handle_ = INVALID_SOCKET;
};

} // namespace net
