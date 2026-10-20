#pragma once

#include "net/socket.h"
#include "server/handler.h"
#include "server/shutdown.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace server {

// Acceptor thread plus K reactor threads (run() calls from the acceptor).
// Each accepted connection is handed to one reactor and stays owned by exactly
// that reactor forever — no connection state is ever shared. Hand-off is a
// socket move over a mutex-protected inbox plus a Wakeup socket; reactors
// never lock anything else.
//
// run() returns after the stop signal, once every reactor drained its queued
// responses or hit the grace cap.
class Server {
public:
    // Throws std::runtime_error when bind/listen fails.
    Server(std::uint16_t port, Handler handler);
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    std::uint16_t port() const { return listener_.localPort(); }

    void run(ShutdownSignal& signal);

    std::chrono::milliseconds idle_timeout{5000};   // per-connection request timeout
    std::chrono::milliseconds shutdown_grace{5000}; // drain budget after stop
    int accept_batch = 64;                          // accepts per poll wakeup
    std::size_t max_queued_bytes = 1u << 20;        // per-connection response backlog
    unsigned threads = 0;                           // reactor count; 0 = hardware_concurrency

private:
    class Reactor; // defined in server.cpp; reactor thread owns its connections

    net::Socket listener_;
    Handler handler_;
};

} // namespace server
