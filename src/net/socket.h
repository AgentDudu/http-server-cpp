#pragma once

#include "net/detail.h"
#include "net/detail_raw.h"

#include <cstddef>

namespace net {

// Move-only owning TCP socket handle. All calls return false / invalid handle on
// failure; the reason is in lastError() (only when the failure came from the OS
// layer — calling on an invalid handle just returns false).
class Socket {
  public:
    Socket() = default;
    explicit Socket(detail::handle_t handle) : handle_(handle) {}
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

    static Socket tcp4(); // invalid handle on failure

    detail::handle_t handle() const { return handle_; }
    bool valid() const { return handle_ != detail::kInvalidHandle; }

    bool setNonBlocking(bool enabled);
    bool setReuseAddress();
    bool bindAny(unsigned short port); // INADDR_ANY; port 0 picks an ephemeral port
    unsigned short localPort() const;  // 0 on failure
    bool listen(int backlog);
    // Accepted socket's blocking mode matches the listener's on both platforms.
    // Invalid handle when none pending (lastError().kind == WouldBlock).
    Socket accept();
    bool connect(const sockaddr_in& address);
    bool close();

    // Blocking convenience for tests and diagnostics. On a non-blocking socket a
    // WouldBlock failure returns false; check lastError().kind. The reactor never
    // uses it; it buffers per-connection instead.
    bool sendAll(const char* data, std::size_t len);
    // What the kernel accepts right now: >0 bytes, -1 error (WouldBlock possible on
    // a non-blocking socket — check lastError().kind). The reactor's write path.
    int sendSome(const char* data, std::size_t len);
    // >0 bytes read, 0 peer closed, -1 error (WouldBlock possible on non-blocking).
    int recvSome(void* buffer, std::size_t capacity);

  private:
    static detail::handle_t detach(Socket& s) {
        const detail::handle_t h = s.handle_;
        s.handle_ = detail::kInvalidHandle;
        return h;
    }

    detail::handle_t handle_ = detail::kInvalidHandle;
};

} // namespace net
