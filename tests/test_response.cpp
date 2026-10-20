#include "doctest.h"

#include "http/headers.h"
#include "http/response.h"
#include "http/status.h"

#include <string>

using http::Headers;
using http::Response;
using http::Status;

TEST_CASE("reason phrase table") {
    CHECK(http::reasonPhrase(Status::OK) == "OK");
    CHECK(http::reasonPhrase(Status::NotFound) == "Not Found");
    CHECK(http::reasonPhrase(Status::RangeNotSatisfiable) == "Range Not Satisfiable");
    CHECK(http::reasonPhrase(static_cast<Status>(418)) == "Unknown");
    CHECK(http::statusOmitsBody(304));
    CHECK(http::statusOmitsBody(204));
    CHECK(http::statusOmitsBody(100));
    CHECK_FALSE(http::statusOmitsBody(200));
}

TEST_CASE("head serialization is byte-exact") {
    Response r(Status::NotFound);
    std::string head;
    REQUIRE(r.serializeHead(head));
    CHECK(head == "HTTP/1.1 404 Not Found\r\n"
                  "Content-Length: 0\r\n"
                  "\r\n");
}

TEST_CASE("content length follows the body unless set explicitly") {
    Response r(Status::OK);
    r.setBody("hello");
    std::string head;
    REQUIRE(r.serializeHead(head));
    CHECK(head == "HTTP/1.1 200 OK\r\n"
                  "Content-Length: 5\r\n"
                  "\r\n");
    CHECK(r.serialize() == "HTTP/1.1 200 OK\r\n"
                           "Content-Length: 5\r\n"
                           "\r\n"
                           "hello");

    r.setHeader("Content-Length", "9"); // explicit wins over derived
    REQUIRE(r.serializeHead(head));
    CHECK(head == "HTTP/1.1 200 OK\r\n"
                  "Content-Length: 9\r\n"
                  "\r\n");
}

TEST_CASE("chunked responses carry no automatic content length") {
    Response r(Status::OK);
    r.setHeader("Transfer-Encoding", "chunked");
    r.setHeader("Content-Type", "text/plain");
    std::string head;
    REQUIRE(r.serializeHead(head));
    CHECK(head == "HTTP/1.1 200 OK\r\n"
                  "Transfer-Encoding: chunked\r\n"
                  "Content-Type: text/plain\r\n"
                  "\r\n");
    CHECK(head.find("Content-Length") == std::string::npos);
}

TEST_CASE("chunk encoder framing") {
    CHECK(http::encodeChunk("hello") == "5\r\nhello\r\n");
    CHECK(http::encodeLastChunk() == "0\r\n\r\n");
    CHECK(http::encodeChunk("a").substr(0, 3) == "1\r\n");
    CHECK(http::encodeChunk(std::string(255, 'x')).substr(0, 4) == "ff\r\n"); // lowercase hex
    const std::string big = http::encodeChunk(std::string(300, 'y'));
    CHECK(big.substr(0, 5) == "12c\r\n");
    CHECK(big.size() == 5 + 300 + 2);
}

TEST_CASE("304 and 204 never claim a length and reject bodies") {
    Response r(Status::NotModified);
    std::string head;
    REQUIRE(r.serializeHead(head));
    CHECK(head.find("Content-Length") == std::string::npos);

    r.setBody("nope");
    CHECK_FALSE(r.serializeHead(head));

    Response n(Status::NoContent);
    n.setBody("nope");
    CHECK_FALSE(n.serializeHead(head));
}

TEST_CASE("header injection is refused at serialization") {
    Response r(Status::OK);
    std::string head;

    r.setHeader("X-Bad", "value\r\nSet-Cookie: evil");
    CHECK_FALSE(r.serializeHead(head));
    r.setHeader("X-Bad", "value\nIn: parts");
    CHECK_FALSE(r.serializeHead(head));
    r.setHeader("X-Bad", std::string("nul\0inside", 10));
    CHECK_FALSE(r.serializeHead(head));
    r.setHeader("X Bad", "space in name");
    CHECK_FALSE(r.serializeHead(head));
    r.setHeader("", "empty name");
    CHECK_FALSE(r.serializeHead(head));

    Response ok(Status::OK);
    ok.setHeader("X-Good", "tab\tallowed in middle");
    CHECK(ok.serializeHead(head));
}

TEST_CASE("headers are ordered, case-insensitive, trimmed") {
    Headers h;
    h.add("Content-Type", "  text/plain  ");
    h.add("X-First", "a\t");
    CHECK(h.get("content-type") != nullptr);
    CHECK(*h.get("CONTENT-TYPE") == "text/plain");
    CHECK(*h.get("x-first") == "a");

    h.set("content-type", "text/html");
    CHECK(*h.get("Content-Type") == "text/html");

    std::size_t seen = 0;
    for (const auto& e : h.entries()) {
        if (e.name == "content-type" || e.name == "Content-Type") {
            ++seen;
        }
    }
    CHECK(seen == 1); // set replaced, did not duplicate

    CHECK_FALSE(h.contains("Missing"));
}

TEST_CASE("field validity rules") {
    CHECK(http::validFieldName("If-None-Match"));
    CHECK(http::validFieldName("X_Cust-om2~!"));
    CHECK_FALSE(http::validFieldName("bad name"));
    CHECK_FALSE(http::validFieldName("bad:name"));
    CHECK(http::validFieldValue("v"));
    CHECK_FALSE(http::validFieldValue(std::string("cr\rhere", 7)));
    CHECK(http::validFieldValue(std::string(1, static_cast<char>(0x80)))); // obs-text
}
