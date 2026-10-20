#include "http/request.h"

#include <cctype>
#include <cstring>

namespace http {
namespace {

bool isOWS(char c) { return c == ' ' || c == '\t'; }

// Value bytes: printable, tab, or extended (obs); never CR/LF/NUL/other CTL.
bool isValueChar(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return u == '\t' || (u >= 0x20 && u != 0x7F);
}

bool equalIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

void RequestParser::feed(std::string_view data) { buf_.append(data); }

void RequestParser::fail(Error e) {
    error_ = e;
    state_ = State::Failed;
}

bool RequestParser::isTokenChar(char c) {
    static constexpr std::string_view kDelims = "()<>@,;:\\\"/[]?={} \t";
    const unsigned char u = static_cast<unsigned char>(c);
    if (u <= 0x20 || u >= 0x7F) {
        return false;
    }
    return kDelims.find(c) == std::string_view::npos;
}

bool RequestParser::parseRequestLine() {
    // RFC 9112: recipient MAY ignore empty lines before the request line.
    while (pos_ < buf_.size()) {
        if (buf_[pos_] == '\r') {
            if (pos_ + 1 >= buf_.size()) {
                return false;
            }
            if (buf_[pos_ + 1] != '\n') {
                fail(Error::BadRequest); // bare CR
                return true;
            }
            pos_ += 2;
        } else if (buf_[pos_] == '\n') {
            ++pos_; // lone LF empty line
        } else {
            break;
        }
    }

    const std::size_t start = pos_;
    head_start_ = start;
    const std::size_t nl = buf_.find("\r\n", start);
    if (buf_.size() - start > limits_.maxHeadBytes) {
        fail(Error::HeadTooLarge);
        return true;
    }
    if (nl == std::string::npos) {
        return false;
    }
    const std::string_view line(buf_.data() + start, nl - start);
    const std::size_t sp1 = line.find(' ');
    const std::size_t sp2 = sp1 == std::string_view::npos ? sp1 : line.find(' ', sp1 + 1);
    if (sp1 == 0 || sp1 == std::string_view::npos || sp2 == std::string_view::npos ||
        line.find(' ', sp2 + 1) != std::string_view::npos) {
        fail(Error::BadRequest);
        return true;
    }
    req_.method = line.substr(0, sp1);
    req_.target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::string_view version = line.substr(sp2 + 1);
    if (version != "HTTP/1.0" && version != "HTTP/1.1") {
        fail(Error::VersionNotSupported);
        return true;
    }
    req_.version = version.substr(5); // "1.0" / "1.1"
    for (std::string_view tok : {req_.method, req_.target}) {
        for (char c : tok) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (u < 0x21 || u > 0x7E) {
                fail(Error::BadRequest);
                return true;
            }
        }
    }
    for (char c : req_.method) {
        if (!isTokenChar(c)) {
            fail(Error::BadRequest);
            return true;
        }
    }
    pos_ = nl + 2;
    scan_ = pos_ >= 2 ? pos_ - 2 : 0; // request line CRLF can start the block terminator
    state_ = State::Headers;
    return true;
}

bool RequestParser::parseHeaderBlock() {
    std::size_t end = buf_.find("\r\n\r\n", scan_);
    scan_ = buf_.size() >= 3 ? buf_.size() - 3 : 0;
    if (end == std::string::npos) {
        if (buf_.size() - head_start_ > limits_.maxHeadBytes) {
            fail(Error::HeadTooLarge); // head still growing past the cap
            return true;
        }
        return false;
    }
    if (end + 4 - head_start_ > limits_.maxHeadBytes) {
        fail(Error::HeadTooLarge);
        return true;
    }
    const std::size_t head_start = head_start_; // lone-LF audit anchor
    // A lone-LF line ending anywhere in the request head is a rejection: bare-LF
    // requests must not be interpreted (smuggling class). A lone-LF terminator
    // (\r\n\n) never matches the \r\n\r\n find above, so it just means NeedMore.
    for (std::size_t i = head_start + 1; i < end + 2; ++i) {
        if (buf_[i] == '\n' && buf_[i - 1] != '\r') {
            fail(Error::BadRequest);
            return true;
        }
    }

    std::size_t count = 0;
    std::size_t ls = pos_;
    const std::size_t body_start = end + 4;
    while (ls < end) {
        const std::size_t le = buf_.find("\r\n", ls); // ls..end block: present
        const std::size_t lend = le == std::string::npos ? end : le;
        if (buf_[ls] == ' ' || buf_[ls] == '\t') {
            fail(Error::BadRequest); // obsolete line folding
            return true;
        }
        const std::size_t colon = buf_.find(':', ls);
        if (colon == std::string::npos || colon > lend || colon == ls) {
            fail(Error::BadRequest);
            return true;
        }
        const std::string_view name(buf_.data() + ls, colon - ls);
        std::size_t vs = colon + 1;
        while (vs < lend && isOWS(buf_[vs])) {
            ++vs;
        }
        std::size_t ve = lend;
        while (ve > vs && isOWS(buf_[ve - 1])) {
            --ve;
        }
        const std::string_view value(buf_.data() + vs, ve - vs);
        for (char c : name) {
            if (!isTokenChar(c)) {
                fail(Error::BadRequest);
                return true;
            }
        }
        for (char c : value) {
            if (!isValueChar(c)) {
                fail(Error::BadRequest);
                return true;
            }
        }
        if (++count > limits_.maxHeaderCount) {
            fail(Error::TooManyHeaders);
            return true;
        }
        const bool is_cl = equalIgnoreCase(name, "content-length");
        const bool is_te = equalIgnoreCase(name, "transfer-encoding");
        if (is_te) {
            fail(Error::BadRequest); // chunked request bodies not supported yet
            return true;
        }
        if (is_cl && req_.headers.contains("content-length")) {
            fail(Error::BadRequest); // duplicate Content-Length
            return true;
        }
        req_.headers.add(std::string(name), std::string(value));
        if (is_cl) {
            std::size_t n = 0;
            std::size_t cl = 0;
            for (char c : value) {
                if (c < '0' || c > '9' || cl > (limits_.maxBodyBytes + 1) / 10) {
                    cl = limits_.maxBodyBytes + 1;
                    break;
                }
                cl = cl * 10 + static_cast<std::size_t>(c - '0');
                ++n;
            }
            if (n == 0 || value.size() != n) {
                fail(Error::BadRequest);
                return true;
            }
            if (cl > limits_.maxBodyBytes) {
                fail(Error::BodyTooLarge);
                return true;
            }
            body_len_ = cl;
        }
        ls = lend + 2;
    }

    pos_ = body_start;
    body_off_ = body_start;
    state_ = body_len_ == 0 ? State::Done : State::Body;
    return true;
}

RequestParser::Result RequestParser::next() {
    for (;;) {
        switch (state_) {
        case State::Failed:
            return Result::Error;
        case State::Done:
            req_.body = body_len_ == 0 ? std::string_view{}
                                       : std::string_view(buf_.data() + body_off_, body_len_);
            return Result::Request;
        case State::RequestLine:
            if (!parseRequestLine()) {
                return state_ == State::Failed ? Result::Error : Result::NeedMore;
            }
            break;
        case State::Headers:
            if (!parseHeaderBlock()) {
                return state_ == State::Failed ? Result::Error : Result::NeedMore;
            }
            break;
        case State::Body:
            if (buf_.size() < body_off_ + body_len_) {
                return Result::NeedMore;
            }
            state_ = State::Done;
            break;
        }
    }
}

bool RequestParser::keepAlive() const {
    if (state_ != State::Done) {
        return false;
    }
    bool has_close = false;
    bool has_keepalive = false;
    if (const std::string* conn = req_.headers.get("connection")) {
        std::size_t s = 0;
        while (s <= conn->size()) {
            std::size_t c = conn->find(',', s);
            if (c == std::string::npos) {
                c = conn->size();
            }
            std::string_view tok(conn->data() + s, c - s);
            while (!tok.empty() && isOWS(tok.front())) {
                tok.remove_prefix(1);
            }
            while (!tok.empty() && isOWS(tok.back())) {
                tok.remove_suffix(1);
            }
            has_close = has_close || equalIgnoreCase(tok, "close");
            has_keepalive = has_keepalive || equalIgnoreCase(tok, "keep-alive");
            s = c + 1;
        }
    }
    if (has_close) {
        return false;
    }
    return req_.version == "1.1" ? true : has_keepalive;
}

void RequestParser::reset() {
    const std::size_t consumed = body_off_ + body_len_;
    buf_.erase(0, consumed);
    pos_ = 0;
    scan_ = 0;
    body_off_ = 0;
    body_len_ = 0;
    req_ = Request{};
    state_ = State::RequestLine;
    error_ = Error::None;
}

} // namespace http
