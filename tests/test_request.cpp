#include "doctest.h"

#include "http/request.h"

#include <string>
#include <string_view>

using http::RequestParser;

namespace {

RequestParser::Result feedAndParse(RequestParser& p, std::string_view input) {
    p.feed(input);
    return p.next();
}

std::string feedAllBytewise(RequestParser& p, std::string_view input) {
    for (std::size_t i = 0; i < input.size(); ++i) {
        p.feed(input.substr(i, 1));
        if (const auto r = p.next(); r != RequestParser::Result::NeedMore) {
            return r == RequestParser::Result::Request ? "request" : "error";
        }
    }
    return "needmore";
}

} // namespace

TEST_CASE("valid minimal GET") {
    RequestParser p;
    CHECK(feedAndParse(p, "GET /a HTTP/1.1\r\n\r\n") == RequestParser::Result::Request);
    const auto& r = p.request();
    CHECK(r.method == "GET");
    CHECK(r.target == "/a");
    CHECK(r.version == "1.1");
    CHECK(r.body.empty());
    CHECK(p.keepAlive()); // HTTP/1.1 default
}

TEST_CASE("fields, headers, body") {
    RequestParser p;
    REQUIRE(feedAndParse(p, "POST /up HTTP/1.0\r\nHost: h\r\nContent-Length: 5\r\nX-A: 1\r\n"
                            "X-A: 2\r\n\r\nhello") == RequestParser::Result::Request);
    const auto& r = p.request();
    CHECK(r.method == "POST");
    CHECK(r.version == "1.0");
    CHECK(r.body == "hello");
    REQUIRE(r.headers.entries().size() == 4);
    CHECK(r.headers.entries()[2].name == "X-A");
    CHECK(r.headers.entries()[2].value == "1");
    CHECK_FALSE(p.keepAlive()); // HTTP/1.0 default
}

TEST_CASE("keep-alive header tokens") {
    RequestParser p11;
    REQUIRE(feedAndParse(p11, "GET / HTTP/1.1\r\nConnection: keep-alive, X\r\n\r\n") ==
            RequestParser::Result::Request);
    CHECK(p11.keepAlive());
    RequestParser pc;
    REQUIRE(feedAndParse(pc, "GET / HTTP/1.1\r\nConnection: close\r\n\r\n") ==
            RequestParser::Result::Request);
    CHECK_FALSE(pc.keepAlive());
    RequestParser p10;
    REQUIRE(feedAndParse(p10, "GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n") ==
            RequestParser::Result::Request);
    CHECK(p10.keepAlive());
}

TEST_CASE("never reads past declared body") {
    RequestParser p;
    REQUIRE(feedAndParse(p, "POST /x HTTP/1.1\r\nContent-Length: 3\r\n\r\nabcdef") ==
            RequestParser::Result::Request);
    CHECK(p.request().body == "abc");
    p.reset();
    // the 3 leftover bytes are not body — they are the next request, and junk.
    CHECK(feedAndParse(p, "\r\n") == RequestParser::Result::Error);
    CHECK(p.error() == RequestParser::Error::BadRequest);
}

TEST_CASE("pipelined requests, one buffer") {
    RequestParser p;
    REQUIRE(feedAndParse(p, "GET /one HTTP/1.1\r\n\r\n"
                            "GET /two HTTP/1.1\r\n\r\n") == RequestParser::Result::Request);
    CHECK(p.request().target == "/one");
    p.reset();
    REQUIRE(p.next() == RequestParser::Result::Request);
    CHECK(p.request().target == "/two");
}

TEST_CASE("byte-at-a-time feed completes") {
    RequestParser p;
    CHECK(feedAllBytewise(p, "GET / HTTP/1.1\r\nA: b\r\n\r\n") == "request");
}

TEST_CASE("empty lines before request line are skipped") {
    RequestParser p;
    CHECK(feedAndParse(p, "\r\n\r\nGET / HTTP/1.1\r\n\r\n") == RequestParser::Result::Request);
    CHECK(p.request().target == "/");
}

TEST_CASE("bad requests are rejected") {
    struct {
        std::string_view name;
        std::string_view input;
        RequestParser::Error want;
    } cases[] = {
        {"no spaces", "GET /\r\n\r\n", RequestParser::Error::BadRequest},
        {"extra space", "GET   / HTTP/1.1\r\n\r\n", RequestParser::Error::BadRequest},
        {"bad version", "GET / HTTP/2.0\r\n\r\n", RequestParser::Error::VersionNotSupported},
        {"version suffix", "GET / HTTP/1.15\r\n\r\n", RequestParser::Error::VersionNotSupported},
        {"method not token", "G[T / HTTP/1.1\r\n\r\n", RequestParser::Error::BadRequest},
        {"header no colon", "GET / HTTP/1.1\r\nbroken\r\n\r\n", RequestParser::Error::BadRequest},
        {"header no name", "GET / HTTP/1.1\r\n: v\r\n\r\n", RequestParser::Error::BadRequest},
        {"obs fold", "GET / HTTP/1.1\r\nA: 1\r\n b\r\n\r\n", RequestParser::Error::BadRequest},
        {"bare LF in headers", "GET / HTTP/1.1\r\nA: 1\nB: 2\r\n\r\n",
         RequestParser::Error::BadRequest},
        {"duplicate CL", "POST / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n",
         RequestParser::Error::BadRequest},
        {"CL plus TE", "POST / HTTP/1.1\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n",
         RequestParser::Error::BadRequest},
        {"TE alone", "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
         RequestParser::Error::BadRequest},
        {"CL not numeric", "POST / HTTP/1.1\r\nContent-Length: 5x\r\n\r\n",
         RequestParser::Error::BadRequest},
        {"CL empty", "POST / HTTP/1.1\r\nContent-Length: \r\n\r\n",
         RequestParser::Error::BadRequest},
    };
    for (const auto& c : cases) {
        CAPTURE(c.name);
        RequestParser p;
        CHECK(feedAndParse(p, c.input) == RequestParser::Result::Error);
        CHECK(p.error() == c.want);
    }
}

TEST_CASE("limits") {
    RequestParser::Limits limits;
    limits.maxHeadBytes = 48;
    limits.maxBodyBytes = 10;
    limits.maxHeaderCount = 2;
    {
        RequestParser p(limits);
        CHECK(feedAndParse(p, "GET / HTTP/1.1\r\nA: aaaa\r\nB: bbbb\r\nC: cccc\r\n\r\n") ==
              RequestParser::Result::Error);
        CHECK(p.error() == RequestParser::Error::TooManyHeaders);
    }
    {
        RequestParser p(limits);
        CHECK(feedAndParse(p, "GET / HTTP/1.1\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\n") ==
              RequestParser::Result::Error);
        CHECK(p.error() == RequestParser::Error::TooManyHeaders);
    }
    {
        RequestParser p(limits);
        CHECK(feedAndParse(p, "GET / HTTP/1.1\r\nLong: " + std::string(60, 'x') + "\r\n\r\n") ==
              RequestParser::Result::Error);
        CHECK(p.error() == RequestParser::Error::HeadTooLarge);
    }
    {
        // head keeps growing with no terminator in sight
        RequestParser p(limits);
        CHECK(feedAndParse(p, "GET / HTTP/1.1\r\nA: " + std::string(100, 'x')) ==
              RequestParser::Result::Error);
        CHECK(p.error() == RequestParser::Error::HeadTooLarge);
    }
    {
        // rejected at header time, before any body byte arrives
        RequestParser p(limits);
        CHECK(feedAndParse(p, "POST / HTTP/1.1\r\nContent-Length: 11\r\n\r\n") ==
              RequestParser::Result::Error);
        CHECK(p.error() == RequestParser::Error::BodyTooLarge);
    }
    {
        RequestParser p(limits);
        CHECK(feedAndParse(p, "POST / HTTP/1.1\r\nContent-Length: 12345\r\n\r\n") ==
              RequestParser::Result::Error);
        CHECK(p.error() == RequestParser::Error::BadRequest);
    }
}
