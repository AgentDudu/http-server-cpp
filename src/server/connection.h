#pragma once

#include "http/request.h"
#include "http/status.h"
#include "net/socket.h"
#include "server/handler.h"
#include "util/time.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace server {

// One keep-alive HTTP connection driven by reactor callbacks. Reads requests
// through http::RequestParser, renders replies through the shared handler,
// buffers outbound bytes — capped, so a slow reader cannot make the server
// hoard responses — and closes when told (idle timeout, parse error,
// Connection: close, or server shutdown).
class Connection {
public:
    using Handler = ::server::Handler;

    Connection(net::Socket socket, const Handler& handler, std::chrono::milliseconds idle,
               std::size_t max_queued);

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    const net::Socket& socket() const { return sock_; }
    bool dead() const { return dead_; }
    bool wantsWrite() const { return want_write_ && !dead_; }
    std::uint64_t deadlineMs() const { return deadline_ms_; }

    void onReadable();
    void onWritable();
    void onTimeout(); // 408 best effort, then close
    void onPeerError() { close(); }
    // Finish what is queued, then close; no further reads.
    void beginShutdown();

private:
    enum class Drain { NeedInput, Throttled, Done };

    Drain drainParser();
    // drainParser + immediate flush, re-draining when a throttle freed up; a
    // throttle that could not flush leaves POLLOUT armed for onWritable.
    Drain drainAndPump();
    void respond(const Reply& reply, bool closeAfter, bool omitBody);
    void flush();
    void close();
    std::size_t pending() const { return out_.size() - out_off_; }

    net::Socket sock_;
    const Handler* handler_;
    std::uint64_t idle_ms_;
    std::size_t max_queued_;
    http::RequestParser parser_;
    std::string out_;
    std::size_t out_off_ = 0;
    bool want_write_ = false;
    bool close_after_write_ = false;
    bool shutting_down_ = false;
    bool dead_ = false;
    std::uint64_t deadline_ms_ = 0;
    char rbuf_[16384];
};

} // namespace server
