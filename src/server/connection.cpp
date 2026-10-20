#include "server/connection.h"

#include "http/response.h"
#include "net/error.h"

namespace server {

Connection::Connection(net::Socket socket, const Handler& handler, std::chrono::milliseconds idle,
                       std::size_t max_queued)
    : sock_(std::move(socket)), handler_(&handler),
      idle_ms_(static_cast<std::uint64_t>(idle.count() < 0 ? 0 : idle.count())),
      max_queued_(max_queued == 0 ? 1 : max_queued) {
    deadline_ms_ = util::monotonicMs() + idle_ms_;
}

void Connection::respond(const Reply& reply, bool closeAfter, bool omitBody) {
    http::Response resp(reply.status);
    for (const auto& entry : reply.headers) {
        resp.setHeader(entry.first, entry.second);
    }
    if (!reply.content_type.empty()) {
        resp.setHeader("Content-Type", reply.content_type);
    }
    if (closeAfter) {
        resp.setHeader("Connection", "close");
    }
    if (omitBody && !http::statusOmitsBody(static_cast<int>(reply.status))) {
        // HEAD: truthful Content-Length, body never written.
        resp.setHeader("Content-Length", std::to_string(reply.body.size()));
    } else if (!omitBody) {
        resp.setBody(reply.body);
    }
    std::string head;
    if (!resp.serializeHead(head)) {
        close(); // header content we must not put on the wire
        return;
    }
    out_ += head;
    if (!omitBody) {
        out_ += reply.body;
    }
    want_write_ = pending() > 0;
    close_after_write_ = close_after_write_ || closeAfter;
}

Connection::Drain Connection::drainParser() {
    for (;;) {
        const auto res = parser_.next();
        if (res == http::RequestParser::Result::NeedMore) {
            return Drain::NeedInput;
        }
        if (res == http::RequestParser::Result::Error) {
            using Err = http::RequestParser::Error;
            http::Status st;
            switch (parser_.error()) {
            case Err::HeadTooLarge:
            case Err::TooManyHeaders:
                st = http::Status::HeaderFieldsTooLarge;
                break;
            case Err::BodyTooLarge:
                st = http::Status::PayloadTooLarge;
                break;
            case Err::VersionNotSupported:
                st = http::Status::HTTPVersionNotSupported;
                break;
            default:
                st = http::Status::BadRequest;
                break;
            }
            // Protocol violation: answer once (best effort), then drop —
            // smuggling candidates never share a stream.
            respond(Reply{st,
                          "text/plain; charset=utf-8",
                          std::string(http::reasonPhrase(st)) + "\n",
                          {}},
                    true, false);
            flush();
            close();
            return Drain::Done;
        }
        const http::Request& req = parser_.request();
        const bool ka = parser_.keepAlive();
        const bool head = req.method == "HEAD";
        Reply reply;
        try {
            reply = (*handler_)(req);
        } catch (...) {
            // Exceptions never cross the reactor; one broken request must not
            // take the process (or the connection's peer) down.
            reply = Reply{http::Status::InternalError,
                          "text/plain; charset=utf-8",
                          "500 Internal Server Error\n",
                          {}};
        }
        parser_.reset();
        served_any_ = true;
        // During shutdown a queued reply (Connection: close) must not strand
        // pipelined requests that were already parsed: keep serving.
        respond(reply, !ka || (shutting_down_ && pending() == 0), head);
        deadline_ms_ = util::monotonicMs() + idle_ms_;
        if (close_after_write_ || dead_) {
            return Drain::Done;
        }
        if (pending() >= max_queued_) {
            // Slow reader: keep remaining pipelined requests buffered in the
            // parser (capped per request), stop reading, wait for POLLOUT.
            return Drain::Throttled;
        }
    }
}

Connection::Drain Connection::drainAndPump() {
    for (;;) {
        if (dead_ || close_after_write_) {
            return Drain::Done;
        }
        const Drain d = drainParser();
        if (d != Drain::Throttled) {
            return d;
        }
        flush(); // may empty the queue entirely — then keep draining
        if (dead_ || pending() < max_queued_) {
            continue;
        }
        return Drain::Throttled; // queue still full: POLLOUT resumes in onWritable
    }
}

void Connection::onReadable() {
    if (dead_) {
        return;
    }
    Drain d = drainAndPump();
    while (d == Drain::NeedInput && !close_after_write_ && !dead_) {
        const int n = sock_.recvSome(rbuf_, sizeof(rbuf_));
        if (n > 0) {
            parser_.feed(std::string_view(rbuf_, static_cast<std::size_t>(n)));
            d = drainAndPump();
            continue;
        }
        if (n < 0 && net::lastError().kind == net::ErrorKind::WouldBlock) {
            break;
        }
        close(); // EOF or error
        return;
    }
    if (dead_) {
        return;
    }
    if (shutting_down_) {
        // Drain what we have; a connection with a half-parsed request may
        // legitimately stay idle until the 408 deadline — the reactor's grace
        // cap bounds it.
        if (!dead_ && close_after_write_ && pending() == 0) {
            close();
        }
        return;
    }
    if (want_write_) {
        flush(); // optimistic: often fits the kernel without a POLLOUT round-trip
    }
    if (!dead_ && !close_after_write_ && pending() < max_queued_) {
        drainAndPump(); // POLLOUT may be spent this round — resume here too
    }
    if (!dead_ && close_after_write_ && pending() == 0) {
        close();
    }
}

void Connection::flush() {
    while (!dead_ && out_off_ < out_.size()) {
        const int n = sock_.sendSome(out_.data() + out_off_, out_.size() - out_off_);
        if (n > 0) {
            out_off_ += static_cast<std::size_t>(n);
            continue;
        }
        if (net::lastError().kind == net::ErrorKind::WouldBlock) {
            want_write_ = true;
            return;
        }
        close();
        return;
    }
    if (!dead_ && pending() == 0) {
        out_.clear();
        out_off_ = 0;
        want_write_ = false;
    }
}

void Connection::onWritable() {
    if (dead_) {
        return;
    }
    flush();
    if (dead_) {
        return;
    }
    if (!close_after_write_ && pending() < max_queued_) {
        drainAndPump(); // room again: resume a throttled drain (POLLOUT may vanish)
    }
    if (!dead_ && close_after_write_ && pending() == 0) {
        close();
    }
}

void Connection::onTimeout() {
    if (dead_) {
        return;
    }
    if (served_any_ && parser_.idle()) {
        // Between requests on a served connection: quiet hang-up, the RFC-
        // prescribed answer to an idle keep-alive stream. 408 is reserved
        // for requests that started and stalled.
        close();
        return;
    }
    respond(
        Reply{
            http::Status::RequestTimeout, "text/plain; charset=utf-8", "408 Request Timeout\n", {}},
        true, false);
    if (!dead_) {
        flush();
        close();
    }
}

void Connection::beginShutdown() {
    shutting_down_ = true;
    if (!want_write_ && pending() == 0) {
        close();
    } else {
        close_after_write_ = true;
    }
}

void Connection::close() {
    dead_ = true;
}

} // namespace server
