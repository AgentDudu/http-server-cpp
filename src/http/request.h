#pragma once

#include "http/headers.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace http {

// A complete HTTP/1.x request. The parser OWNS every byte here: views into
// its working buffer die when a later feed() reallocates it, so the parser
// copies method/target/version/body the moment a request completes — a
// Request handed out by next() stays valid across reset() and further feeds.
struct Request {
    std::string method;
    std::string target;
    std::string version; // "1.0" or "1.1"
    Headers headers;     // owned copies, parse order preserved
    std::string body;    // empty unless Content-Length was present
};

// Push parser: feed() raw bytes, call next() until Request/Error. Enforces the
// head/body limits and rejects smuggling-prone shapes: bare-LF line endings,
// obsolete header folding, duplicate Content-Length, Content-Length together
// with Transfer-Encoding, non-token method/name characters, unknown versions.
class RequestParser {
public:
    struct Limits {
        std::size_t maxHeadBytes = 64 * 1024;
        std::size_t maxBodyBytes = 8 * 1024 * 1024;
        std::size_t maxHeaderCount = 100;
    };

    enum class Result { NeedMore, Request, Error };
    enum class Error {
        None,
        BadRequest,          // syntax, folding, duplicate CL, CL+TE, bad chars
        HeadTooLarge,        // head exceeds maxHeadBytes
        TooManyHeaders,      // header count exceeds maxHeaderCount
        BodyTooLarge,        // declared Content-Length exceeds maxBodyBytes
        VersionNotSupported, // anything but HTTP/1.0 / HTTP/1.1
    };

    explicit RequestParser(Limits limits) : limits_(limits) {}
    RequestParser() = default;

    void feed(std::string_view data);

    // Call repeatedly after NeedMore. After Request, call reset() to consume it
    // (leftover buffered bytes belong to the next pipelined request).
    Result next();

    const Request& request() const { return req_; }
    Error error() const { return error_; }

    // Valid only after Result::Request.
    bool keepAlive() const;
    // No request bytes since the last reset(): the stream waits for a fresh
    // request line. An idle keep-alive timeout should then just close (RFC
    // 9112), not answer 408 to a request that was never started.
    bool idle() const { return state_ == State::RequestLine && buf_.empty(); }

    void reset();

    std::size_t bufferedBytes() const { return buf_.size() - pos_; }

private:
    enum class State { RequestLine, Headers, Body, Done, Failed };

    bool parseRequestLine();
    bool parseHeaderBlock();
    void fail(Error e);
    static bool isTokenChar(char c);

    Limits limits_;
    std::string buf_;
    std::size_t pos_ = 0;        // first unparsed byte (request line / header scan cursor)
    std::size_t scan_ = 0;       // offset already searched for the header terminator
    std::size_t head_start_ = 0; // first byte of the current request line
    State state_ = State::RequestLine;
    Error error_ = Error::None;
    Request req_;
    std::size_t body_off_ = 0;
    std::size_t body_len_ = 0;
};

} // namespace http
