#include "server/connection.h"

#include "http/response.h"
#include "http/status.h"
#include "net/error.h"

namespace server {

Connection::Connection(net::Socket socket, const Handler& handler,
                       std::chrono::milliseconds idle)
    : sock_(std::move(socket)), handler_(&handler),
      idle_ms_(static_cast<std::uint64_t>(idle.count() < 0 ? 0 : idle.count())) {
    deadline_ms_ = util::monotonicMs() + idle_ms_;
}

void Connection::respond(http::Status status, std::string_view body, bool closeAfter) {
    http::Response resp(status);
    resp.setHeader("Content-Type", "text/plain; charset=utf-8");
    if (closeAfter) {
        resp.setHeader("Connection", "close");
    }
    resp.setBody(std::string(body));
    out_ += resp.serialize();
    want_write_ = out_off_ < out_.size();
    close_after_write_ = close_after_write_ || closeAfter;
}

bool Connection::drainParser() {
    for (;;) {
        const auto res = parser_.next();
        if (res == http::RequestParser::Result::NeedMore) {
            return true;
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
            // Protocol violation: answer once (best effort), then drop the
            // connection — smuggling candidates never share a stream.
            respond(st, std::string(http::reasonPhrase(st)) + "\n", true);
            flush();
            close();
            return false;
        }
        const bool ka = parser_.keepAlive();
        const std::string body = (*handler_)(parser_.request());
        parser_.reset();
        respond(http::Status::OK, body, !ka || shutting_down_);
        deadline_ms_ = util::monotonicMs() + idle_ms_;
        if (close_after_write_) {
            return false;
        }
    }
}

void Connection::onReadable() {
    if (dead_) {
        return;
    }
    bool more = drainParser();
    while (more) {
        if (close_after_write_ || dead_) {
            more = false;
            break;
        }
        const int n = sock_.recvSome(rbuf_, sizeof(rbuf_));
        if (n > 0) {
            parser_.feed(std::string_view(rbuf_, static_cast<std::size_t>(n)));
            more = drainParser();
            continue;
        }
        if (n < 0 && net::lastError().kind == net::ErrorKind::WouldBlock) {
            more = false;
            break;
        }
        close(); // EOF or error
        return;
    }
    if (shutting_down_ && !want_write_) {
        close();
        return;
    }
    if (want_write_) {
        flush(); // optimistic: often fits the kernel without a POLLOUT round-trip
    }
    if (!dead_ && close_after_write_ && out_off_ == out_.size()) {
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
    if (!dead_ && out_off_ == out_.size()) {
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
    if (!dead_ && close_after_write_ && out_off_ == out_.size()) {
        close();
    }
}

void Connection::onTimeout() {
    if (dead_) {
        return;
    }
    respond(http::Status::RequestTimeout, "408 Request Timeout\n", true);
    flush();
    close();
}

void Connection::beginShutdown() {
    shutting_down_ = true;
    if (!want_write_ && out_off_ == out_.size()) {
        close();
    } else {
        close_after_write_ = true;
    }
}

void Connection::close() { dead_ = true; }

} // namespace server
