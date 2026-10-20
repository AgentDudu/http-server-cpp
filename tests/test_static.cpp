#include "doctest.h"

#include "http/date.h"
#include "http/request.h"
#include "server/static_file.h"

#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

void writeFile(const fs::path& p, std::string_view content) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

struct TestTree {
    fs::path root;
    fs::path outside;
    TestTree() {
        root = fs::temp_directory_path() / ("hs_static_" + std::to_string(::rand()));
        outside = root.parent_path() / ("hs_outside_" + std::to_string(::rand()));
        fs::create_directories(root);
        writeFile(root / "index.html", "<html>root</html>");
        writeFile(root / "a.txt", "alpha");
        writeFile(root / "ten.txt", "0123456789");
        writeFile(root / "nested" / "index.html", "nested-index");
        writeFile(root / "noext", "raw");
        writeFile(root / "UPPER.HTML", "<html>upper</html>");
        writeFile(outside / "secret.txt", "SECRET");
    }
    ~TestTree() {
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::remove_all(outside, ec);
    }
};

http::Request makeRequest(std::string_view method, std::string_view target) {
    http::Request req;
    req.method = method;
    req.target = target;
    req.version = "1.1";
    return req;
}

} // namespace

TEST_CASE("static files: serving and content types") {
    TestTree tree;
    server::StaticFiles files(tree.root);

    auto r = files(makeRequest("GET", "/a.txt"));
    CHECK(r.status == http::Status::OK);
    CHECK(r.body == "alpha");
    CHECK(r.content_type == "text/plain; charset=utf-8");

    r = files(makeRequest("GET", "/"));
    CHECK(r.body == "<html>root</html>");
    CHECK(r.content_type == "text/html; charset=utf-8");

    r = files(makeRequest("GET", "/nested/"));
    CHECK(r.body == "nested-index");

    r = files(makeRequest("GET", "/noext"));
    CHECK(r.body == "raw");
    CHECK(r.content_type == "application/octet-stream");

    r = files(makeRequest("GET", "/UPPER.HTML"));
    CHECK(r.status == http::Status::OK); // exists as written

    r = files(makeRequest("GET", "/a.txt?query=1#frag"));
    CHECK(r.body == "alpha"); // query and fragment stripped

    r = files(makeRequest("GET", "/a%2etxt"));
    CHECK(r.body == "alpha"); // percent-decoded before resolution
}

TEST_CASE("static files: traversal and hostile targets refuse with 404") {
    TestTree tree;
    server::StaticFiles files(tree.root);

    const char* hostile[] = {
        "/../secret.txt",
        "/%2e%2e/secret.txt",
        "/%2E%2E%2Fsecret.txt",
        "/nested/../../secret.txt",
        "/%2e%2e/%2e%2e/secret.txt",
        "/\\..\\secret.txt",   // backslash smuggling
        "/C:/Windows/win.ini", // absolute on Windows, odd name on posix
        "/%c0%afx",            // overlong encoding: just a weird name
        "/%zz",                // invalid escape
        "/a%00b",              // decoded NUL
        "/nested/../nested/../secret.txt",
    };
    for (const char* target : hostile) {
        const auto r = files(makeRequest("GET", target));
        CHECK_MESSAGE(r.status == http::Status::NotFound,
                      target << " -> " << static_cast<int>(r.status));
        CHECK_MESSAGE(r.body.find("SECRET") == std::string::npos, target);
    }

    fs::path out;
    CHECK_FALSE(files.resolve("", out));
    CHECK_FALSE(files.resolve("relative/path", out)); // must start with '/'
    CHECK(files.resolve("/a.txt", out));
    // root_ is canonical; on these platforms temp is too, but don't assume:
    CHECK(out == fs::canonical(tree.root) / "a.txt");
}

TEST_CASE("static files: methods and misses") {
    TestTree tree;
    server::StaticFiles files(tree.root);

    auto r = files(makeRequest("POST", "/a.txt"));
    CHECK(r.status == http::Status::MethodNotAllowed);
    r = files(makeRequest("DELETE", "/a.txt"));
    CHECK(r.status == http::Status::MethodNotAllowed);
    r = files(makeRequest("HEAD", "/missing"));
    CHECK(r.status == http::Status::NotFound);

    // HEAD on an existing file keeps the honest length (connection drops body)
    r = files(makeRequest("HEAD", "/a.txt"));
    CHECK(r.status == http::Status::OK);
    CHECK(r.body == "alpha");
}

TEST_CASE("mimeFor table") {
    CHECK(server::mimeFor("page.html") == "text/html; charset=utf-8");
    CHECK(server::mimeFor("PAGE.HTM") == "text/html; charset=utf-8");
    CHECK(server::mimeFor("app.JS") == "text/javascript; charset=utf-8");
    CHECK(server::mimeFor("photo.jpeg") == "image/jpeg");
    CHECK(server::mimeFor("style.css") == "text/css; charset=utf-8");
    CHECK(server::mimeFor("archive.tar") == "application/octet-stream");
    CHECK(server::mimeFor("noext") == "application/octet-stream");
}

// ---- conditional requests and ranges (G) ------------------------------------

namespace {

const std::string* replyHeader(const server::Reply& r, std::string_view name) {
    for (const auto& h : r.headers) {
        if (h.first == name) {
            return &h.second;
        }
    }
    return nullptr;
}

http::Request condRequest(std::string_view target, std::string_view name, std::string value,
                          std::string_view method = "GET") {
    auto req = makeRequest(method, target);
    if (!name.empty()) {
        req.headers.set(std::string(name), std::move(value));
    }
    return req;
}

std::string getEtag(server::StaticFiles& files, const http::Request& req) {
    const auto r = files(req);
    const std::string* etag = replyHeader(r, "ETag");
    REQUIRE(etag != nullptr);
    return *etag;
}

} // namespace

TEST_CASE("conditional GET: validators present, 304 on match") {
    TestTree tree;
    server::StaticFiles files(tree.root);

    const auto base = files(makeRequest("GET", "/ten.txt"));
    REQUIRE(base.status == http::Status::OK);
    const std::string etag = getEtag(files, makeRequest("GET", "/ten.txt"));
    CHECK(etag.front() == '"');
    REQUIRE(replyHeader(base, "Last-Modified") != nullptr);
    CHECK(http::parseHttpDate(*replyHeader(base, "Last-Modified")).has_value());
    REQUIRE(replyHeader(base, "Accept-Ranges") != nullptr);
    CHECK(*replyHeader(base, "Accept-Ranges") == "bytes");

    const auto hit = files(condRequest("/ten.txt", "If-None-Match", etag));
    CHECK(hit.status == http::Status::NotModified);
    CHECK(hit.body.empty());
    CHECK(hit.content_type.empty()); // 304 carries no representation
    CHECK(replyHeader(hit, "ETag") != nullptr);

    // weak comparison (RFC 7232 3.2) and star and lists
    CHECK(files(condRequest("/ten.txt", "If-None-Match", "W/" + etag)).status ==
          http::Status::NotModified);
    CHECK(files(condRequest("/ten.txt", "If-None-Match", "*")).status == http::Status::NotModified);
    CHECK(files(condRequest("/ten.txt", "If-None-Match", "\"other\", " + etag)).status ==
          http::Status::NotModified);
    CHECK(files(condRequest("/ten.txt", "If-None-Match", "\"stale-etag\"")).status ==
          http::Status::OK);
    CHECK(files(condRequest("/ten.txt", "If-None-Match", etag, "HEAD")).status ==
          http::Status::NotModified);
    // missing file never takes a 304
    CHECK(files(condRequest("/missing", "If-None-Match", "*")).status == http::Status::NotFound);
}

TEST_CASE("conditional GET: If-Modified-Since, If-None-Match wins") {
    TestTree tree;
    server::StaticFiles files(tree.root);

    const std::string future = http::formatHttpDate(std::time(nullptr) + 3600);
    CHECK(files(condRequest("/ten.txt", "If-Modified-Since", future)).status ==
          http::Status::NotModified);
    CHECK(files(condRequest("/ten.txt", "If-Modified-Since", "Sun, 06 Nov 1994 08:49:37 GMT"))
              .status == http::Status::OK);
    CHECK(files(condRequest("/ten.txt", "If-Modified-Since", "not-a-date")).status ==
          http::Status::OK);

    // RFC 7232 3.3: with If-None-Match present it alone decides — a future
    // If-Modified-Since must not rescue a mismatched If-None-Match into 304.
    auto both = makeRequest("GET", "/ten.txt");
    both.headers.set("If-None-Match", "\"nope\"");
    both.headers.set("If-Modified-Since", future);
    CHECK(files(both).status == http::Status::OK);
}

TEST_CASE("ranges: single-range 206, 416, and ignore-what-we-do-not-parse") {
    TestTree tree;
    server::StaticFiles files(tree.root);
    const std::string etag = getEtag(files, makeRequest("GET", "/ten.txt"));

    struct Case {
        const char* range;
        http::Status status;
        std::string body;
        const char* content_range;
    };
    const Case cases[] = {
        {"bytes=0-4", http::Status::PartialContent, "01234", "bytes 0-4/10"},
        {"bytes=5-", http::Status::PartialContent, "56789", "bytes 5-9/10"},
        {"bytes=-3", http::Status::PartialContent, "789", "bytes 7-9/10"},
        {"bytes=9-99", http::Status::PartialContent, "9", "bytes 9-9/10"},
        {"bytes=0-0", http::Status::PartialContent, "0", "bytes 0-0/10"},
        // unsatisfiable: first-pos past the end
        {"bytes=10-", http::Status::RangeNotSatisfiable, "", "bytes */10"},
        {"bytes=100-200", http::Status::RangeNotSatisfiable, "", "bytes */10"},
        // unparsable or unsupported -> plain 200 with the whole entity
        {"bytes=0-1,3-4", http::Status::OK, "0123456789", nullptr}, // multi-range
        {"bytes=abc", http::Status::OK, "0123456789", nullptr},
        {"bytes=5-3", http::Status::OK, "0123456789", nullptr}, // reversed
        {"bytes=-", http::Status::OK, "0123456789", nullptr},   // suffix-less
        {"bytes=-0", http::Status::OK, "0123456789", nullptr},  // zero suffix
        {"items=0-1", http::Status::OK, "0123456789", nullptr}, // other unit
        {"bytes 0-1", http::Status::OK, "0123456789", nullptr}, // bad syntax
    };
    for (const auto& c : cases) {
        auto req = condRequest("/ten.txt", "Range", c.range);
        const auto r = files(req);
        INFO(c.range);
        CHECK(r.status == c.status);
        if (c.content_range != nullptr) {
            REQUIRE(replyHeader(r, "Content-Range") != nullptr);
            CHECK(*replyHeader(r, "Content-Range") == c.content_range);
        }
        if (c.status == http::Status::PartialContent) {
            CHECK(r.body == c.body);
            CHECK(replyHeader(r, "ETag") != nullptr);
        } else if (c.status == http::Status::OK) {
            CHECK(r.body == c.body);
        }
    }

    // HEAD ignores Range (it is not a range request per RFC 9110 14.2)
    const auto head = files(condRequest("/ten.txt", "Range", "bytes=0-3", "HEAD"));
    CHECK(head.status == http::Status::OK);

    // If-Range: matching validator applies the range, anything else serves all
    auto ir_ok = condRequest("/ten.txt", "Range", "bytes=0-4");
    ir_ok.headers.set("If-Range", etag);
    CHECK(files(ir_ok).status == http::Status::PartialContent);

    auto ir_stale = condRequest("/ten.txt", "Range", "bytes=0-4");
    ir_stale.headers.set("If-Range", "\"changed\"");
    const auto all = files(ir_stale);
    CHECK(all.status == http::Status::OK);
    CHECK(all.body == "0123456789");

    auto ir_date = condRequest("/ten.txt", "Range", "bytes=0-4");
    ir_date.headers.set("If-Range",
                        *replyHeader(files(makeRequest("GET", "/ten.txt")), "Last-Modified"));
    CHECK(files(ir_date).status == http::Status::PartialContent);

    // weak ETag never matches If-Range (strong comparison)
    auto ir_weak = condRequest("/ten.txt", "Range", "bytes=0-4");
    ir_weak.headers.set("If-Range", "W/" + etag);
    CHECK(files(ir_weak).status == http::Status::OK);
}

TEST_CASE("405 carries Allow") {
    TestTree tree;
    server::StaticFiles files(tree.root);
    const auto r = files(makeRequest("POST", "/ten.txt"));
    CHECK(r.status == http::Status::MethodNotAllowed);
    REQUIRE(replyHeader(r, "Allow") != nullptr);
    CHECK(*replyHeader(r, "Allow") == "GET, HEAD");
}
