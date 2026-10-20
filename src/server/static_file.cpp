#include "server/static_file.h"

#include "http/date.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
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

std::string trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return std::string(s);
}

// Wall-clock seconds for last_write_time, bridged with paired now()s —
// std::clock_cast is not in every supported stdlib.shortcut: microsecond drift
// on a header with one-second resolution.
std::time_t mtimeSeconds(const fs::path& p, std::error_code& ec) {
    const auto lwt = fs::last_write_time(p, ec);
    if (ec) {
        return 0;
    }
    const auto wall = std::chrono::system_clock::now() + (lwt - std::chrono::file_clock::now());
    return std::chrono::duration_cast<std::chrono::seconds>(wall.time_since_epoch()).count();
}

std::string makeEtag(std::uintmax_t size, std::time_t mtime) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "\"%llx-%llx\"", static_cast<unsigned long long>(size),
                  static_cast<unsigned long long>(static_cast<long long>(mtime)));
    return buf;
}

// RFC 7232 3.2: If-None-Match uses weak comparison — "W/" prefixes ignored.
bool etagListMatches(std::string_view inm, std::string_view etag) {
    if (trim(inm) == "*") {
        return true;
    }
    for (std::size_t pos = 0; pos < inm.size();) {
        const auto comma = inm.find(',', pos);
        std::string_view item =
            inm.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
        if (item.size() > 2 && item[0] == 'W' && item[1] == '/') {
            item = item.substr(2);
        }
        if (trim(item) == etag) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        pos = comma + 1;
    }
    return false;
}

// RFC 7233 3.2: If-Range matches strongly — a W/ validator never matches.
bool ifRangeMatches(std::string_view ir, std::string_view etag, std::time_t lm) {
    const std::string v = trim(ir);
    if (!v.empty() && v.front() == '"') {
        return v == etag;
    }
    const auto t = http::parseHttpDate(v);
    return t && *t == lm;
}

struct ByteRange {
    enum Kind { Ignore, Satisfied, Unsatisfiable } kind = Ignore;
    std::uintmax_t begin = 0;
    std::uintmax_t end = 0; // inclusive
};

// Single byte-range only (RFC 9110 14.1.1 semantics); multi-ranges, syntax
// errors and unknown units mean "ignore the Range header" (serve 200).
ByteRange parseRange(std::string_view value, std::uintmax_t size) {
    ByteRange r;
    constexpr std::string_view unit = "bytes=";
    if (value.substr(0, unit.size()) != unit || value.size() == unit.size()) {
        return r;
    }
    std::string_view spec = value.substr(unit.size());
    if (spec.find(',') != std::string_view::npos) {
        return r; // multiple ranges: not served, 200 whole entity instead
    }
    const auto dash = spec.find('-');
    if (dash == std::string_view::npos || dash + 1 > spec.size()) {
        return r;
    }
    const auto digits = [](std::string_view t) {
        if (t.empty() || t.size() > 20) {
            return std::make_pair(false, std::uintmax_t{0});
        }
        std::uintmax_t v = 0;
        for (char c : t) {
            if (c < '0' || c > '9') {
                return std::make_pair(false, std::uintmax_t{0});
            }
            v = v * 10 + static_cast<std::uintmax_t>(c - '0');
        }
        return std::make_pair(true, v);
    };
    const auto [bOk, begin] = digits(spec.substr(0, dash));
    const bool hasEnd = dash + 1 < spec.size();
    const auto [eOk, end] =
        hasEnd ? digits(spec.substr(dash + 1)) : std::make_pair(true, std::uintmax_t{0});
    if (!bOk && !eOk) {
        return r; // "-" alone
    }
    if (!bOk) { // suffix form "-N"
        if (dash != 0 || !hasEnd || end == 0) {
            return r;
        }
        if (size == 0) {
            r.kind = ByteRange::Unsatisfiable;
            return r;
        }
        r.kind = ByteRange::Satisfied;
        r.begin = end >= size ? 0 : size - end;
        r.end = size - 1;
        return r;
    }
    if (!eOk || (hasEnd && end < begin)) {
        return r;
    }
    r.kind = begin < size ? ByteRange::Satisfied : ByteRange::Unsatisfiable;
    r.begin = begin;
    r.end = hasEnd ? (size == 0 ? 0 : (end >= size ? size - 1 : end)) : (size == 0 ? 0 : size - 1);
    return r;
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
        r.status = http::Status::MethodNotAllowed;
        r.body = "405 Method Not Allowed\n";
        r.headers = {{"Allow", "GET, HEAD"}};
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
    const std::uintmax_t size = fs::file_size(p, ec);
    if (ec) {
        r.status = http::Status::NotFound; // vanished between stat and size
        r.body = "404 Not Found\n";
        return r;
    }
    std::error_code tec;
    const std::time_t lm = mtimeSeconds(p, tec);
    const std::string etag = makeEtag(size, lm);
    const std::string lmText = http::formatHttpDate(lm);
    r.headers = {{"ETag", etag}, {"Last-Modified", lmText}, {"Accept-Ranges", "bytes"}};

    // Conditional GET (RFC 7232): If-None-Match alone decides when present;
    // If-Modified-Since only applies without it.
    bool not_modified = false;
    if (const std::string* inm = req.headers.get("If-None-Match")) {
        not_modified = etagListMatches(*inm, etag);
    } else if (const std::string* ims = req.headers.get("If-Modified-Since")) {
        const auto t = http::parseHttpDate(*ims);
        not_modified = t && !tec && lm <= *t;
    }
    if (not_modified) {
        r.status = http::Status::NotModified;
        r.content_type.clear(); // 304 carries no representation headers
        return r;
    }

    // Range applies to GET only; If-Range mismatch or anything unparsable
    // means "serve the whole entity" (RFC 7233).
    std::size_t from = 0;
    std::size_t len = static_cast<std::size_t>(size);
    bool partial = false;
    std::string contentRange;
    if (req.method == "GET") {
        if (const std::string* rangeHdr = req.headers.get("Range")) {
            bool apply = true;
            if (const std::string* ir = req.headers.get("If-Range")) {
                apply = ifRangeMatches(*ir, etag, lm);
            }
            const ByteRange br = apply ? parseRange(*rangeHdr, size) : ByteRange{};
            if (br.kind == ByteRange::Unsatisfiable) {
                r.status = http::Status::RangeNotSatisfiable;
                r.body = "416 Range Not Satisfiable\n";
                r.headers = {
                    {"Content-Range",
                     "bytes */" + std::to_string(static_cast<unsigned long long>(size))},
                    {"ETag", etag},
                    {"Last-Modified", lmText},
                };
                return r;
            }
            if (br.kind == ByteRange::Satisfied) {
                partial = true;
                from = static_cast<std::size_t>(br.begin);
                len = static_cast<std::size_t>(br.end - br.begin + 1);
                contentRange = "bytes " +
                               std::to_string(static_cast<unsigned long long>(br.begin)) + "-" +
                               std::to_string(static_cast<unsigned long long>(br.end)) + "/" +
                               std::to_string(static_cast<unsigned long long>(size));
            }
        }
    }

    std::ifstream f(p, std::ios::binary);
    if (!f) {
        r.status = http::Status::NotFound; // vanished between stat and open
        r.body = "404 Not Found\n";
        r.headers.clear();
        return r;
    }
    // shortcut: whole-file read then slice; bounded streaming is a later phase.
    std::string body;
    {
        std::ostringstream buf;
        buf << f.rdbuf();
        body = buf.str();
    }
    if (from > body.size()) {
        from = body.size(); // raced with a truncate between stat and read
    }
    len = std::min(len, body.size() - from);
    if (partial) {
        if (body.size() < from + len) {
            // file shrank after stat: the promised range no longer exists.
            r.status = http::Status::RangeNotSatisfiable;
            r.body = "416 Range Not Satisfiable\n";
            r.headers = {
                {"Content-Range", "bytes */" + std::to_string(body.size())},
                {"ETag", makeEtag(body.size(), lm)},
                {"Last-Modified", lmText},
            };
            return r;
        }
        r.status = http::Status::PartialContent;
        r.headers.emplace_back("Content-Range", contentRange);
    }
    body.erase(0, from);
    body.resize(len);
    r.content_type = std::string(mimeFor(p.filename().string()));
    r.body = std::move(body);
    return r;
}

} // namespace server
