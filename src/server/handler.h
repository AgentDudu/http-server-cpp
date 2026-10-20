#pragma once

#include "http/request.h"
#include "http/status.h"

#include <functional>
#include <string>

namespace server {

// What a handler returns. HEAD is handled by the connection: the body is
// dropped but Content-Length stays honest.
struct Reply {
    http::Status status = http::Status::OK;
    std::string content_type = "text/plain; charset=utf-8";
    std::string body;
};

using Handler = std::function<Reply(const http::Request&)>;

} // namespace server
