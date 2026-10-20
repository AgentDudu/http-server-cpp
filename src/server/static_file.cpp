#include "server/static_file.h"

#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

int hexVal(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Strict: '%' must introduce exactly two hex digits; a decoded NUL refuses.
bool percentDecode(std::string_view in, std::string& out) {
    out.clear();
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c != '%') {
            out.push_back(c);
            continue;
        }
        if (i + 2 >= in.size()) {
            return false;
        }
        const int hi = hexVal(in[i + 1]);
        const int lo = hexVal(in[i + 2]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        const char v = static_cast<char>(hi * 16 + lo);
        if (v == '\0') {
            return false;
        }
        out.push_back(v);
        i += 2;
    }
    return true;
}

std::string toLower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

} // namespace

namespace server {

std::string_view mimeFor(std::string_view filename) {
    const auto dot = filename.find_last_of('.');
    const std::string ext =
        dot == std::string_view::npos ? std::string() : toLower(filename.substr(dot + 1));
    if (ext == "html" || ext == "htm") {
        return "text/html; charset=utf-8";
    }
    if (ext == "css") {
        return "text/css; charset=utf-8";
    }
    if (ext == "js") {
        return "text/javascript; charset=utf-8";
    }
    if (ext == "json" || ext == "map") {
        return "application/json";
    }
    if (ext == "txt") {
        return "text/plain; charset=utf-8";
    }
    if (ext == "png") {
        return "image/png";
    }
    if (ext == "jpg" || ext == "jpeg") {
        return "image/jpeg";
    }
    if (ext == "gif") {
        return "image/gif";
    }
    if (ext == "svg") {
        return "image/svg+xml";
    }
    if (ext == "ico") {
        return "image/vnd.microsoft.icon";
    }
    if (ext == "pdf") {
        return "application/pdf";
    }
    if (ext == "wasm") {
        return "application/wasm";
    }
    return "application/octet-stream";
}

StaticFiles::StaticFiles(std::filesystem::path root) {
    std::error_code ec;
    root_ = fs::canonical(root, ec);
    if (ec || !fs::is_directory(root_)) {
        throw fs::filesystem_error("static root is not a directory", root, ec);
    }
}

bool StaticFiles::contained(const fs::path& candidate) const {
    std::error_code ec;
    fs::path real = fs::weakly_canonical(candidate, ec); // resolves symlink prefixes
    if (ec) {
        real = candidate.lexically_normal();
    }
    auto rel = real.lexically_relative(root_);
    if (rel.empty() || *rel.begin() == "..") {
        return false;
    }
#ifdef _WIN32
    // Windows path equality is case-insensitive; compare folded, or a
    // differently-cased root prefix would look like an escape.
    auto folded =
        fs::path(toLower(real.string())).lexically_relative(fs::path(toLower(root_.string())));
    if (folded.empty() || *folded.begin() == "..") {
        return false;
    }
#endif
    return true;
}

bool StaticFiles::resolve(std::string_view target, fs::path& out) const {
    if (target.empty() || target.front() != '/') {
        return false;
    }
    const auto q = target.find_first_of("?#");
    target = target.substr(0, q == std::string_view::npos ? target.size() : q);

    std::string decoded;
    if (!percentDecode(target, decoded)) {
        return false;
    }
    if (decoded.find('\\') != std::string::npos) { // Windows separator smuggling
        return false;
    }
    for (const char c : decoded) {
        if (static_cast<unsigned char>(c) < 0x20) {
            return false;
        }
    }

    const std::string rel = decoded.substr(1); // drop the leading '/'
    try {
        const fs::path relp(rel);
        if (relp.is_absolute() || relp.has_root_path()) {
            return false; // "/C:/Windows/..." must not replace the root on Windows
        }
        const fs::path candidate = (root_ / relp).lexically_normal();
        if (!contained(candidate)) {
            return false;
        }
        out = candidate;
        return true;
    } catch (const fs::filesystem_error&) {
        // Windows paths are wide: invalid UTF-8 (e.g. "%c0%af") throws on
        // conversion — a hostile name, not a server error.
        return false;
    }
}

Reply StaticFiles::operator()(const http::Request& req) const {
    Reply r;
    if (req.method != "GET" && req.method != "HEAD") {
        // shortcut: no Allow header — Reply carries no per-reply headers,
        // upgrade when a handler needs custom headers.
        r.status = http::Status::MethodNotAllowed;
        r.body = "405 Method Not Allowed\n";
        return r;
    }
    fs::path p;
    std::error_code ec;
    if (!resolve(req.target, p)) {
        r.status = http::Status::NotFound;
        r.body = "404 Not Found\n";
        return r;
    }
    if (fs::is_directory(p, ec)) {
        p /= "index.html";
        if (!contained(p)) {
            r.status = http::Status::NotFound;
            r.body = "404 Not Found\n";
            return r;
        }
    }
    if (!fs::is_regular_file(p, ec)) {
        r.status = http::Status::NotFound;
        r.body = "404 Not Found\n";
        return r;
    }
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        r.status = http::Status::NotFound; // vanished between stat and open
        r.body = "404 Not Found\n";
        return r;
    }
    // shortcut: whole-file read; bounded streaming lands with Range support.
    std::string body;
    {
        std::ostringstream buf;
        buf << f.rdbuf();
        body = buf.str();
    }
    r.content_type = std::string(mimeFor(p.filename().string()));
    r.body = std::move(body);
    return r;
}

} // namespace server
