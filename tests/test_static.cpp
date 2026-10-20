#include "doctest.h"

#include "http/request.h"
#include "server/static_file.h"

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
