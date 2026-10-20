// Multi-reactor threading coverage (the CI TSan job runs this across real
// threads). Round-robin hand-off means 8+ fresh connections land on all four
// reactors; per-connection semantics must survive intact: keep-alive,
// pipelining, drain-on-stop from every reactor, and a stop() requested while
// the acceptor sits in poll() — the EINTR path that once double-joined a
// reactor and escaped run() with pthread_join EINVAL.
#include "doctest.h"

#include "http/request.h"
#include "net/context.h"
#include "net/socket.h"
#include "server/server.h"
#include "server/shutdown.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

class ManualSignal final : public server::ShutdownSignal {
public:
    bool stopRequested() const override { return stop.load(); }
    std::chrono::milliseconds pollCap() const override { return std::chrono::milliseconds(20); }
    std::atomic_bool stop{false};
};

server::Reply okReply(std::string body) {
    server::Reply r;
    r.body = std::move(body);
    return r;
}

sockaddr_in loopback(unsigned short port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7F000001);
    addr.sin_port = htons(port);
    return addr;
}

net::Socket connectTo(unsigned short port) {
    auto s = net::Socket::tcp4();
    if (s.valid()) {
        s.connect(loopback(port));
    }
#ifdef _WIN32
    DWORD v = 3000;
    ::setsockopt(s.handle(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&v), sizeof(v));
#else
    timeval tv{3, 0};
    ::setsockopt(s.handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    return s;
}

// Read one full response (head + Content-Length body), keeping any trailing
// bytes of coalesced responses in `carry`.
std::string readResponse(net::Socket& s, std::string& carry) {
    std::string buf = std::move(carry);
    carry.clear();
    char chunk[256];
    for (;;) {
        const std::size_t head_end = buf.find("\r\n\r\n");
        if (head_end != std::string::npos) {
            const std::size_t cl_pos = buf.find("Content-Length: ");
            std::size_t total = head_end + 4;
            if (cl_pos != std::string::npos && cl_pos < head_end) {
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

std::string readResponse(net::Socket& s) {
    std::string ignored;
    return readResponse(s, ignored);
}

bool sendReq(net::Socket& s, std::string_view req) {
    return s.sendAll(req.data(), req.size());
}

} // namespace

TEST_CASE("threading: keep-alive across four reactors") {
    net::Context ctx;
    ManualSignal sig;
    std::atomic<unsigned> serves{0};
    server::Server srv(0, [&serves](const http::Request&) {
        return okReply("r" + std::to_string(serves.fetch_add(1)));
    });
    srv.threads = 4;
    const unsigned short port = srv.port();
    std::thread runner([&] { srv.run(sig); });

    std::vector<net::Socket> conns;
    for (int i = 0; i < 12; ++i) {
        auto c = connectTo(port);
        REQUIRE(c.valid());
        REQUIRE(sendReq(c, "GET /a HTTP/1.1\r\nHost: x\r\n\r\n"));
        conns.push_back(std::move(c));
    }
    std::string carry;
    for (auto& c : conns) {
        CHECK(readResponse(c, carry).find("HTTP/1.1 200 OK") != std::string::npos);
    }
    for (auto& c : conns) { // second request: keep-alive on its owning reactor
        REQUIRE(sendReq(c, "GET /b HTTP/1.1\r\nHost: x\r\n\r\n"));
    }
    for (auto& c : conns) {
        CHECK(readResponse(c, carry).find("HTTP/1.1 200 OK") != std::string::npos);
    }
    sig.stop = true;
    runner.join();
}

TEST_CASE("threading: stop drains connections held by every reactor") {
    net::Context ctx;
    ManualSignal sig;
    server::Server srv(0, [](const http::Request&) { return okReply("drain"); });
    srv.threads = 4;
    const unsigned short port = srv.port();
    std::thread runner([&] { srv.run(sig); });

    std::vector<net::Socket> conns;
    for (int i = 0; i < 8; ++i) { // round-robin lands on all four reactors
        auto c = connectTo(port);
        REQUIRE(c.valid());
        REQUIRE(sendReq(c, "GET / HTTP/1.1\r\nHost: x\r\n\r\n"));
        std::string carry;
        CHECK(readResponse(c, carry).find("200") != std::string::npos);
        conns.push_back(std::move(c));
    }
    sig.stop = true;
    runner.join(); // after every reactor drained its queue
    for (auto& c : conns) {
        char b[1];
        CHECK(c.recvSome(b, 1) == 0); // every reactor closed its connections
    }
}

TEST_CASE("threading: pipelined storms and fresh connections cross reactors") {
    net::Context ctx;
    ManualSignal sig;
    server::Server srv(0, [](const http::Request&) { return okReply("storm"); });
    srv.threads = 4;
    srv.idle_timeout = std::chrono::seconds(5);
    const unsigned short port = srv.port();
    std::thread runner([&] { srv.run(sig); });

    const std::string burst = [] {
        std::string b;
        for (int i = 0; i < 5; ++i) {
            b += "GET /s HTTP/1.1\r\nHost: x\r\n\r\n";
        }
        return b;
    }();
    std::vector<net::Socket> conns;
    std::vector<int> rounds;
    for (int round = 0; round < 3; ++round) {
        auto c = connectTo(port); // fresh client while the others are busy
        REQUIRE(c.valid());
        conns.push_back(std::move(c));
        rounds.push_back(0);
        for (std::size_t i = 0; i < conns.size(); ++i) {
            REQUIRE(sendReq(conns[i], burst));
            ++rounds[i];
        }
    }
    for (std::size_t i = 0; i < conns.size(); ++i) {
        std::string carry;
        for (int j = 0; j < rounds[i] * 5; ++j) {
            CHECK(readResponse(conns[i], carry).find("200") != std::string::npos);
        }
    }
    sig.stop = true;
    runner.join();
}

TEST_CASE("threading: stop while the acceptor sits idle in poll") {
    net::Context ctx;
    ManualSignal sig;
    server::Server srv(0, [](const http::Request&) { return okReply("sig"); });
    srv.threads = 3;
    const unsigned short port = srv.port();
    std::thread runner([&] { srv.run(sig); });

    auto c = connectTo(port); // one connection so drain work exists at stop
    REQUIRE(c.valid());
    REQUIRE(sendReq(c, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"));
    CHECK(readResponse(c).find("200") != std::string::npos);

    std::this_thread::sleep_for(std::chrono::milliseconds(60)); // poll is idle
    sig.stop.store(true); // set from another thread, like a signal handler would
    const auto t0 = std::chrono::steady_clock::now();
    runner.join();
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3));
    // Linux additionally proves the EINTR flavor of this path: the SIGINT
    // smoke in tools/asan-inside.sh asserts exit 0 (it caught the historical
    // EINTR -> double-join regression).
}
