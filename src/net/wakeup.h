#pragma once

#include "net/socket.h"

#include <utility>

namespace net {

// Cross-thread wake-up for a poll loop: notify() (any thread) makes the thread
// blocked in pollSockets() return by making readHandle() readable. POSIX uses a
// socketpair; Windows uses a loopback pair — it is just a socket either way.
class Wakeup {
public:
    Wakeup() = default; // invalid on failure (lastError() has the reason)
    static Wakeup create();

    detail::handle_t readHandle() const { return in_.handle(); }
    bool valid() const { return in_.valid() && out_.valid(); }

    void notify(); // fire-and-forget; coalesces
    void drain();  // consume pending wake-ups

private:
    Wakeup(Socket in, Socket out) : in_(std::move(in)), out_(std::move(out)) {}
    Socket in_;
    Socket out_;
};

} // namespace net
