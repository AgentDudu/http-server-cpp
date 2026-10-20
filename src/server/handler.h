#pragma once

#include "http/request.h"
#include "http/status.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace server {

// What a handler returns. HEAD is handled by the connection: the body is
// dropped but Content-Length stays honest.
struct Reply {
    http::Status status = http::Status::OK;
    std::string content_type = "text/plain; charset=utf-8"; // empty = omit header
    std::string body;
    // Extra response headers (ETag, Content-Range, ...) emitted before
    // Content-Type; values must already be header-field safe.
    std::vector<std::pair<std::string, std::string>> headers;
};

using Handler = std::function<Reply(const http::Request&)>;

} // namespace server
