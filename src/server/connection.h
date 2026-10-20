#pragma once

#include "http/request.h"
#include "http/status.h"
#include "net/socket.h"
#include "util/time.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace server {

// One keep-alive HTTP connection driven by reactor callbacks. Reads requests
// through http::RequestParser, renders responses through the shared handler,
// buffers outbound bytes, and closes when told (idle timeout, parse error,
// Connection: close, or server shutdown).
class Connection {
  public:
    // Renders the body for one request. Must not retain request views.
    using Handler = std::function<std::string(const http::Request&)>;

    Connection(net::Socket socket, const Handler& handler, std::chrono::milliseconds idle);

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
    void respond(http::Status status, std::string_view body, bool closeAfter);
    // Maps http::RequestParser::Error; returns false once the connection is dying.
    bool drainParser();
    void flush();
    void close();

    net::Socket sock_;
    const Handler* handler_;
    std::uint64_t idle_ms_;
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
