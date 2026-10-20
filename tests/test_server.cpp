#include "doctest.h"

#include "http/request.h"
#include "net/context.h"
#include "net/error.h"
#include "net/socket.h"
#include "server/server.h"
#include "server/shutdown.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace {

class ManualSignal final : public server::ShutdownSignal {
  public:
    bool stopRequested() const override { return stop.load(); }
    std::chrono::milliseconds pollCap() const override { return std::chrono::milliseconds(20); }
    std::atomic_bool stop{false};
};

sockaddr_in loopback(unsigned short port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7F000001);
    addr.sin_port = htons(port);
    return addr;
}

void setRecvTimeout(net::Socket& s, int ms) {
#ifdef _WIN32
    DWORD v = static_cast<DWORD>(ms);
    ::setsockopt(s.handle(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&v),
                 sizeof(v));
#else
    timeval tv{ms / 1000, static_cast<long>(ms % 1000) * 1000L};
    ::setsockopt(s.handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

net::Socket connectTo(unsigned short port) {
    auto s = net::Socket::tcp4();
    if (s.valid()) {
        s.connect(loopback(port));
    }
    setRecvTimeout(s, 3000);
    return s;
}

// One full response: head + Content-Length bytes, or whatever arrived before
// timeout/EOF.
std::string readResponse(net::Socket& s) {
    std::string buf;
    char chunk[256];
    for (;;) {
        const int n = s.recvSome(chunk, sizeof(chunk));
        if (n <= 0) {
            return buf;
        }
        buf.append(chunk, static_cast<std::size_t>(n));
        const std::size_t head_end = buf.find("\r\n\r\n");
        if (head_end == std::string::npos) {
            continue;
        }
        const std::size_t cl_pos = buf.find("Content-Length: ");
        if (cl_pos == std::string::npos || cl_pos > head_end) {
            return buf; // our server always sets Content-Length; nothing to wait for
        }
        std::size_t cl = 0;
        std::size_t i = cl_pos + 16;
        while (i < buf.size() && buf[i] >= '0' && buf[i] <= '9') {
            cl = cl * 10 + static_cast<std::size_t>(buf[i++] - '0');
        }
        if (buf.size() >= head_end + 4 + cl) {
            return buf;
        }
    }
}

bool seesEof(net::Socket& s) {
    char b[8];
    return s.recvSome(b, sizeof(b)) == 0; // 0 = clean EOF; -1 = timeout/error
}

bool sendReq(net::Socket& s, std::string_view req) {
    return s.sendAll(req.data(), req.size());
}

} // namespace

TEST_CASE("200 over keep-alive, then Connection: close, then clean stop") {
    net::Context ctx;
    ManualSignal sig;
    server::Server srv(0, [](const http::Request& req) {
        return req.target == "/close" ? std::string("bye\n") : std::string("ok\n");
    });
    const unsigned short port = srv.port();
    REQUIRE(port > 0);
    std::thread runner([&] { srv.run(sig); });

    auto c = connectTo(port);
    REQUIRE(c.valid());
    REQUIRE(sendReq(c, "GET /a HTTP/1.1\r\n\r\n"));
    const std::string r1 = readResponse(c);
    CHECK(r1.find("HTTP/1.1 200 OK") != std::string::npos);
    CHECK(r1.find("Content-Length: 3") != std::string::npos);
    CHECK(r1.find("\r\n\r\nok\n") != std::string::npos);

    // same socket: keep-alive worked; this one asks to close
    REQUIRE(sendReq(c, "GET /close HTTP/1.1\r\nConnection: close\r\n\r\n"));
    const std::string r2 = readResponse(c);
    CHECK(r2.find("HTTP/1.1 200 OK") != std::string::npos);
    CHECK(r2.find("Connection: close") != std::string::npos);
    CHECK(seesEof(c)); // server closed after flush

    sig.stop.store(true);
    runner.join();
}

TEST_CASE("idle connection receives 408 then gets closed") {
    net::Context ctx;
    ManualSignal sig;
    server::Server srv(0, [](const http::Request&) { return std::string("ok\n"); });
    srv.idle_timeout = std::chrono::milliseconds(150);
    const unsigned short port = srv.port();
    std::thread runner([&] { srv.run(sig); });

    auto c = connectTo(port);
    REQUIRE(c.valid());
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const std::string r = readResponse(c);
    CHECK(r.find("408") != std::string::npos);
    CHECK(seesEof(c));

    // server still serving: fresh connection works
    auto c2 = connectTo(port);
    REQUIRE(c2.valid());
    REQUIRE(sendReq(c2, "GET / HTTP/1.1\r\n\r\n"));
    CHECK(readResponse(c2).find("200") != std::string::npos);

    sig.stop.store(true);
    runner.join();
}

TEST_CASE("malformed request gets 400 and the connection drops") {
    net::Context ctx;
    ManualSignal sig;
    server::Server srv(0, [](const http::Request&) { return std::string("ok\n"); });
    const unsigned short port = srv.port();
    std::thread runner([&] { srv.run(sig); });

    auto c = connectTo(port);
    REQUIRE(c.valid());
    REQUIRE(sendReq(c, "BOGUS\r\n\r\n"));
    const std::string r = readResponse(c);
    CHECK(r.find("400 Bad Request") != std::string::npos);
    CHECK(seesEof(c));

    sig.stop.store(true);
    runner.join();
}

TEST_CASE("stop while connections are idle returns promptly") {
    net::Context ctx;
    ManualSignal sig;
    server::Server srv(0, [](const http::Request&) { return std::string("ok\n"); });
    const unsigned short port = srv.port();
    std::thread runner([&] { srv.run(sig); });

    auto c = connectTo(port);
    REQUIRE(c.valid());
    // One served request guarantees the reactor accepted this connection; it is
    // then idle when the stop lands.
    REQUIRE(sendReq(c, "GET / HTTP/1.1\r\n\r\n"));
    CHECK(readResponse(c).find("200") != std::string::npos);
    sig.stop.store(true);
    runner.join(); // idle connection is closed by beginShutdown; no grace wait
    CHECK(seesEof(c));
}
