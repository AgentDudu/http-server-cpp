#pragma once

#include "net/socket.h"
#include "server/connection.h"
#include "server/handler.h"
#include "server/shutdown.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace server {

// Single-threaded poll reactor: one listener, N keep-alive connections, one
// fixed handler. run() returns after the stop signal, once queued responses
// drained or the grace expired. Threading (worker hand-off) comes later.
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

private:
    void reap();

    net::Socket listener_;
    Handler handler_;
    std::vector<std::unique_ptr<Connection>> conns_;
};

} // namespace server
