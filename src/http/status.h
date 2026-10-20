#pragma once

#include <string_view>

namespace http {

// RFC 7231/7232/7233 subset used by this server.
enum class Status : int {
    Continue = 100,
    SwitchingProtocols = 101,
    OK = 200,
    Created = 201,
    Accepted = 202,
    NoContent = 204,
    PartialContent = 206,
    MovedPermanently = 301,
    Found = 302,
    NotModified = 304,
    TemporaryRedirect = 307,
    PermanentRedirect = 308,
    BadRequest = 400,
    Unauthorized = 401,
    Forbidden = 403,
    NotFound = 404,
    MethodNotAllowed = 405,
    RequestTimeout = 408,
    LengthRequired = 411,
    PayloadTooLarge = 413,
    URITooLong = 414,
    RangeNotSatisfiable = 416,
    HeaderFieldsTooLarge = 431,
    InternalError = 500,
    NotImplemented = 501,
    ServiceUnavailable = 503,
    HTTPVersionNotSupported = 505,
};

// Reason phrase for the status line; "Unknown" for codes outside the table.
std::string_view reasonPhrase(Status status);

// Statuses that must not carry a message body (and therefore no Content-Length).
bool statusOmitsBody(int code);

} // namespace http
