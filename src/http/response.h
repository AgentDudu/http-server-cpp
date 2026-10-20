#pragma once

#include "http/headers.h"
#include "http/status.h"

#include <string>
#include <string_view>

namespace http {

// An HTTP/1.1 response message. Head serialization is strict: invalid names or
// values (CTLs, CR, LF, NUL) fail the call instead of emitting a smugglable wire.
// Unless Transfer-Encoding declares chunked, a Content-Length is implied by the
// body and written automatically when not set explicitly.
class Response {
  public:
    explicit Response(Status status = Status::OK) : status_(status) {}

    Status status() const { return status_; }
    void setStatus(Status status) { status_ = status; }

    void setHeader(std::string name, std::string value) {
        headers_.set(std::move(name), std::move(value));
    }
    void addHeader(std::string name, std::string value) {
        headers_.add(std::move(name), std::move(value));
    }
    const Headers& headers() const { return headers_; }

    void setBody(std::string body) { body_ = std::move(body); }
    const std::string& body() const { return body_; }

    // false + untouched `out` when any header is invalid, or a body-less status
    // carries a body. Includes the terminating empty line.
    bool serializeHead(std::string& out) const;
    // Head + body. Empty string if head serialization fails.
    std::string serialize() const;

  private:
    bool chunked() const;

    Status status_;
    Headers headers_;
    std::string body_;
};

// chunk-size (lowercase hex) CRLF data CRLF
std::string encodeChunk(std::string_view data);
// The zero-size final chunk.
inline std::string encodeLastChunk() { return "0\r\n\r\n"; }

} // namespace http
