#include "http/status.h"

namespace http {

std::string_view reasonPhrase(Status status) {
    switch (status) {
    case Status::Continue:
        return "Continue";
    case Status::SwitchingProtocols:
        return "Switching Protocols";
    case Status::OK:
        return "OK";
    case Status::Created:
        return "Created";
    case Status::Accepted:
        return "Accepted";
    case Status::NoContent:
        return "No Content";
    case Status::PartialContent:
        return "Partial Content";
    case Status::MovedPermanently:
        return "Moved Permanently";
    case Status::Found:
        return "Found";
    case Status::NotModified:
        return "Not Modified";
    case Status::TemporaryRedirect:
        return "Temporary Redirect";
    case Status::PermanentRedirect:
        return "Permanent Redirect";
    case Status::BadRequest:
        return "Bad Request";
    case Status::Unauthorized:
        return "Unauthorized";
    case Status::Forbidden:
        return "Forbidden";
    case Status::NotFound:
        return "Not Found";
    case Status::MethodNotAllowed:
        return "Method Not Allowed";
    case Status::RequestTimeout:
        return "Request Timeout";
    case Status::LengthRequired:
        return "Length Required";
    case Status::PayloadTooLarge:
        return "Payload Too Large";
    case Status::URITooLong:
        return "URI Too Long";
    case Status::RangeNotSatisfiable:
        return "Range Not Satisfiable";
    case Status::HeaderFieldsTooLarge:
        return "Header Fields Too Large";
    case Status::InternalError:
        return "Internal Server Error";
    case Status::NotImplemented:
        return "Not Implemented";
    case Status::ServiceUnavailable:
        return "Service Unavailable";
    case Status::HTTPVersionNotSupported:
        return "HTTP Version Not Supported";
    }
    return "Unknown";
}

bool statusOmitsBody(int code) {
    return code < 200 || code == 204 || code == 304;
}

} // namespace http
