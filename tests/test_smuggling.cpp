// Server-level request-framing corpus: every stream here is a smuggling
// candidate (or a slow-body variant). Contract per case: the exact sequence
// of status codes the server puts on the wire, and that a protocol error is
// the LAST thing on the stream — a rejected request must never leave a
// second response stranded behind it, and the connection always drops.
#include "doctest.h"

#include "http/request.h"
#include "net/context.h"
#include "net/socket.h"
#include "server/server.h"
#include "server/shutdown.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace {

constexpr int kSdSend =
#ifdef _WIN32
    SD_SEND;
#else
    SHUT_WR;
#endif

class ManualSignal final : public server::ShutdownSignal {
public:
    bool stopRequested() const override { return stop.load(); }
    std::chrono::milliseconds pollCap() const override { return std::chrono::milliseconds(20); }
    std::atomic_bool stop{false};
};

class FrameServer {
public:
    explicit FrameServer(int idle_ms) {
        srv.idle_timeout = std::chrono::milliseconds(idle_ms);
        runner = std::thread([this] { srv.run(sig); });
    }
    ~FrameServer() {
        sig.stop = true;
        runner.join();
    }
    server::Server srv{0, [this](const http::Request&) {
                           ++calls;
                           server::Reply r;
                           r.body = "ok\n";
                           return r;
                       }};
    unsigned short port() const { return srv.port(); }
    std::atomic_int calls{0};

private:
    ManualSignal sig;
    std::thread runner;
};

sockaddr_in loopback(unsigned short port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7F000001);
    addr.sin_port = htons(port);
    return addr;
}

net::Socket connectTo(unsigned short port, int recv_ms) {
    auto s = net::Socket::tcp4();
    if (s.valid()) {
        s.connect(loopback(port));
    }
#ifdef _WIN32
    DWORD v = static_cast<DWORD>(recv_ms);
    ::setsockopt(s.handle(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&v), sizeof(v));
#else
    timeval tv{recv_ms / 1000, static_cast<long>(recv_ms % 1000) * 1000L};
    ::setsockopt(s.handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    return s;
}

std::string readSome(net::Socket& s, std::string& carry) {
    std::string buf = std::move(carry);
    carry.clear();
    char chunk[256];
    for (;;) {
        const std::size_t head_end = buf.find("\r\n\r\n");
        if (head_end != std::string::npos) {
            const std::size_t cl_pos = buf.find("Content-Length: ");
            std::size_t total = std::string::npos;
            if (cl_pos == std::string::npos || cl_pos > head_end) {
                total = head_end + 4;
            } else {
                std::size_t cl = 0;
                std::size_t i = cl_pos + 16;
                while (i < buf.size() && buf[i] >= '0' && buf[i] <= '9') {
                    cl = cl * 10 + static_cast<std::size_t>(buf[i++] - '0');
                }
                total = head_end + 4 + cl;
            }
            if (buf.size() >= total) {
                carry = buf.substr(total);
                buf.resize(total);
                return buf;
            }
        }
        const int n = s.recvSome(chunk, sizeof(chunk));
        if (n <= 0) {
            return buf;
        }
        buf.append(chunk, static_cast<std::size_t>(n));
    }
}

// "200,400" style code trace; "" when the peer closed before answering.
std::string codesOf(net::Socket& s, int max_responses = 3) {
    std::string carry;
    std::string out;
    for (int i = 0; i < max_responses; ++i) {
        const std::string r = readSome(s, carry);
        if (r.size() < 12 || r.compare(0, 5, "HTTP/") != 0) {
            break;
        }
        out += out.empty() ? "" : ",";
        out += r.substr(9, 3);
    }
    return out;
}

bool seesEof(net::Socket& s) {
    char b[8];
    return s.recvSome(b, sizeof(b)) == 0;
}

struct Case {
    const char* name;
    std::string stream;
    const char* codes; // expected status trace
    bool closes;       // server must drop the stream after that trace
};

// Keep embedded NULs: std::string(const char*) would truncate at them.
#define RAW(s) std::string((s), sizeof(s) - 1)

} // namespace

TEST_CASE("framing corpus: reject once, then the stream is over") {
    net::Context ctx;
    FrameServer srv(700);

    const Case table[] = {
        {"CL+TE",
         "POST /a HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n"
         "Transfer-Encoding: chunked\r\n\r\n1\r\nX\r\n0\r\n\r\n",
         "400", true},
        {"TE-only",
         "POST /a HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
         "1\r\nX\r\n0\r\n\r\n",
         "400", true},
        {"TE-valueless-tab",
         "POST /a HTTP/1.1\r\nHost: x\r\nTransfer-Encoding:\tchunked\r\n\r\n"
         "1\r\nX\r\n0\r\n\r\n",
         "400", true},
        {"TE-duplicated",
         "POST /a HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n"
         "Transfer-Encoding: chunked\r\n\r\n1\r\nX\r\n0\r\n\r\n",
         "400", true},
        {"CL-duplicated-same",
         "POST /a HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n"
         "Content-Length: 5\r\n\r\n12345",
         "400", true},
        {"CL-plus-signed", "POST /a HTTP/1.1\r\nHost: x\r\nContent-Length: +5\r\n\r\n12345", "400",
         true},
        {"CL-garbage", "POST /a HTTP/1.1\r\nHost: x\r\nContent-Length: 5x\r\n\r\n12345", "400",
         true},
        {"CL-spaces-internal", "POST /a HTTP/1.1\r\nHost: x\r\nContent-Length: 1 2\r\n\r\n12",
         "400", true},
        {"bare-LF-between-requests",
         "GET /a HTTP/1.1\nHost: x\r\n\r\nGET /b HTTP/1.1\r\nHost: x\r\n\r\n", "400", true},
        {"NUL-in-header-value", //
         RAW("GET /a HTTP/1.1\r\nHost: x\0y\r\n\r\n"), "400", true},
        {"obs-fold", "GET /a HTTP/1.1\r\nHost: x\r\nX: 1\r\n A: 2\r\n\r\n", "400", true},
        {"valid-then-poison",
         "GET /a HTTP/1.1\r\nHost: x\r\n\r\nPOST /b HTTP/1.1\r\nHost: x\r\n"
         "Content-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n",
         "200,400", true},
        {"poison-hides-behind-valid",
         RAW("GET /a HTTP/1.1\r\nHost: x\r\n\r\nGET /b HTTP/1.1\r\nHost: x\0z\r\n\r\n"), "200,400",
         true},
        {"CL-leading-zero", "POST /a HTTP/1.1\r\nHost: x\r\nContent-Length: 05\r\n\r\n12345", "200",
         false},
        {"partial-body-then-FIN", "POST /a HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nab", "",
         true},
    };

    for (const Case& c : table) {
        INFO("case=" << c.name);
        auto s = connectTo(srv.port(), 2500);
        REQUIRE(s.valid());
        REQUIRE(s.sendAll(c.stream.data(), c.stream.size()));
        if (std::string(c.name) == "partial-body-then-FIN") {
            ::shutdown(s.handle(), kSdSend); // EOF mid-body
        }
        const std::string got = codesOf(s);
        CHECK_MESSAGE(got == c.codes, std::string(c.name) << " got [" << got << "]");
        if (c.closes) {
            CHECK_MESSAGE(seesEof(s), c.name << " must drop the stream");
        }
    }
}

TEST_CASE("slow body: silence past the idle budget gets 408 and a drop") {
    net::Context ctx;
    FrameServer srv(400); // 400 ms per-request budget

    auto s = connectTo(srv.port(), 3000);
    REQUIRE(s.valid());
    const std::string head = "POST /a HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n\r\n";
    REQUIRE(s.sendAll(head.data(), head.size()));
    const char b = 'x';
    REQUIRE(s.sendAll(&b, 1)); // one byte in, then go quiet
    const std::string got = codesOf(s, 1);
    CHECK(got == "408");
    CHECK(seesEof(s));
}

TEST_CASE("slow body that finishes inside the budget is served normally") {
    net::Context ctx;
    FrameServer srv(700);

    auto s = connectTo(srv.port(), 3000);
    REQUIRE(s.valid());
    const std::string head = "POST /a HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n";
    REQUIRE(s.sendAll(head.data(), head.size()));
    for (const char* b = "12345"; *b; ++b) {
        REQUIRE(s.sendAll(b, 1));
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // < 700 budget
    }
    CHECK(codesOf(s, 1) == "200");
    CHECK(srv.calls == 1);
}

TEST_CASE("no phantom handler calls: rejected streams never reach the handler") {
    net::Context ctx;
    FrameServer srv(700);

    auto s = connectTo(srv.port(), 2500);
    REQUIRE(s.valid());
    const std::string stream =
        "GET /a HTTP/1.1\r\nHost: x\r\n\r\n" // 1 handler call
        "GET /b HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"  // rejected: no call, stream ends
        "GET /c HTTP/1.1\r\nHost: x\r\n\r\n"; // never parsed — stream dropped
    REQUIRE(s.sendAll(stream.data(), stream.size()));
    CHECK(codesOf(s) == "200,400");
    CHECK(seesEof(s));
    CHECK(srv.calls == 1); // /c must never execute the handler
}
