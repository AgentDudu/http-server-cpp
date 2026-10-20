#include "http/response.h"

#include <cstdio>

namespace http {

namespace {
bool valueEqualsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z')
            ca = static_cast<char>(ca + 32);
        if (cb >= 'A' && cb <= 'Z')
            cb = static_cast<char>(cb + 32);
        if (ca != cb) {
            return false;
        }
    }
    return true;
}
} // namespace

bool Response::chunked() const {
    const std::string* te = headers_.get("Transfer-Encoding");
    return te != nullptr && valueEqualsIgnoreCase(*te, "chunked");
}

bool Response::serializeHead(std::string& out) const {
    const int code = static_cast<int>(status_);
    if (statusOmitsBody(code) && !body_.empty()) {
        return false; // RFC 7230: 204/304/1xx must not carry a body
    }

    std::string head =
        "HTTP/1.1 " + std::to_string(code) + " " + std::string(reasonPhrase(status_)) + "\r\n";

    bool haveLength = headers_.get("Content-Length") != nullptr;
    for (const auto& entry : headers_.entries()) {
        if (!validFieldName(entry.name) || !validFieldValue(entry.value)) {
            return false;
        }
        head += entry.name + ": " + entry.value + "\r\n";
    }

    if (!haveLength && !chunked() && !statusOmitsBody(code)) {
        head += "Content-Length: " + std::to_string(body_.size()) + "\r\n";
    }

    head += "\r\n";
    out = std::move(head);
    return true;
}

std::string Response::serialize() const {
    std::string head;
    if (!serializeHead(head)) {
        return {};
    }
    head += body_;
    return head;
}

std::string encodeChunk(std::string_view data) {
    char sizeLine[32];
    std::snprintf(sizeLine, sizeof(sizeLine), "%llx\r\n",
                  static_cast<unsigned long long>(data.size()));
    std::string out(sizeLine);
    out.append(data);
    out += "\r\n";
    return out;
}

} // namespace http
