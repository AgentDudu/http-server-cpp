// 60-second churn soak: many short-lived, keep-alive, slow-read, pipelined
// and aborting clients against the real threaded server while the process
// RSS is sampled. Growth past the warm-up window means per-connection state
// is not being released. Runs only when HTTP_SERVER_SOAK_SECONDS is set —
// it is minutes of wall time, not a unit test.
#include "doctest.h"

#include "http/request.h"
#include "net/context.h"
#include "net/socket.h"
#include "server/server.h"
#include "server/shutdown.h"
#include "server/static_file.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__

namespace {

std::atomic_bool g_stop{false};
std::atomic_int g_completed{0};
std::atomic_int g_errors{0};

long long rssKb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            return std::atoll(line.c_str() + 6);
        }
    }
    return -1;
}

sockaddr_in loopback(unsigned short port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7F000001);
    addr.sin_port = htons(port);
    return addr;
}

void setRecvTimeout(net::Socket& s, int ms) {
    timeval tv{ms / 1000, static_cast<long>(ms % 1000) * 1000L};
    ::setsockopt(s.handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

bool drainSome(net::Socket& s, int bytes_needed) {
    char buf[4096];
    int got = 0;
    while (got < bytes_needed) {
        const int n = s.recvSome(buf, sizeof(buf));
        if (n <= 0) {
            return false;
        }
        got += n;
    }
    return true;
}

// One client iteration per worker; every mode is churn-sensitive on purpose.
void clientLoop(unsigned short port, std::mt19937& rng) {
    std::uniform_int_distribution<int> mode(0, 4);
    while (!g_stop.load()) {
        auto s = net::Socket::tcp4();
        if (!s.valid()) {
            ++g_errors;
            continue;
        }
        s.connect(loopback(port));
        setRecvTimeout(s, 5000);
        bool ok = false;
        switch (mode(rng)) {
        case 0: { // short connection: one request, close
            const std::string r = "GET /f HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
            ok = s.sendAll(r.data(), r.size()) && drainSome(s, 40);
            break;
        }
        case 1: { // keep-alive: 10 sequential requests
            const std::string r = "GET /f HTTP/1.1\r\nHost: x\r\n\r\n";
            ok = true;
            for (int i = 0; i < 10 && ok; ++i) {
                ok = s.sendAll(r.data(), r.size()) && drainSome(s, 40);
            }
            break;
        }
        case 2: { // slow reader: nibble the response, then abandon it
            const std::string r = "GET /big HTTP/1.1\r\nHost: x\r\n\r\n";
            ok = s.sendAll(r.data(), r.size());
            char b[64];
            for (int i = 0; i < 3 && ok; ++i) {
                ok = s.recvSome(b, sizeof(b)) > 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            ok = true; // abandoning is the point; no expectation on completeness
            break;
        }
        case 3: { // abort: half a request line, then FIN
            const std::string r = "GET /f HTT";
            ok = s.sendAll(r.data(), r.size());
            ::shutdown(s.handle(), SHUT_WR);
            ok = true;
            break;
        }
        default: { // pipelined burst: 5 requests in one write, read all
            std::string r;
            for (int i = 0; i < 5; ++i) {
                r += "GET /f HTTP/1.1\r\nHost: x\r\n\r\n";
            }
            ok = s.sendAll(r.data(), r.size()) && drainSome(s, 5 * 40);
            break;
        }
        }
        // socket closes here — half-read buffers, queued responses and all.
        ok ? ++g_completed : ++g_errors;
    }
}

} // namespace

TEST_CASE("60 s churn soak keeps process RSS flat") {
    const char* env = std::getenv("HTTP_SERVER_SOAK_SECONDS");
    const int seconds = env ? std::atoi(env) : 0;
    if (seconds <= 0) {
        MESSAGE("soak skipped (set HTTP_SERVER_SOAK_SECONDS=60 to run)");
        return;
    }

    net::Context ctx;
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "hs_soak_root";
    fs::create_directories(root);
    {
        std::ofstream f(root / "f");
        f << "hello soak\n";
        std::ofstream big(root / "big");
        big << std::string(1u << 18, 'B'); // 256 KiB to give slow readers a backlog
    }

    server::StaticFiles files(root);
    server::Server srv(0, files);
    srv.threads = 4;
    std::atomic_bool stop_server{false};
    class Sig final : public server::ShutdownSignal {
    public:
        bool stopRequested() const override { return stop; }
        std::chrono::milliseconds pollCap() const override { return std::chrono::milliseconds(20); }
        std::atomic_bool stop{false};
    } sig;
    std::thread runner([&] { srv.run(sig); });

    std::vector<std::thread> workers;
    for (int i = 0; i < 24; ++i) {
        workers.emplace_back([i, port = srv.port()] {
            std::mt19937 rng(static_cast<unsigned>(i) * 7919u + 13u);
            clientLoop(port, rng);
        });
    }

    long long base = -1;
    long long peak_after_base = -1;
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(seconds);
    const auto sample_from = start + std::chrono::seconds(seconds / 3 > 10 ? seconds / 3 : 10);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (std::chrono::steady_clock::now() < sample_from) {
            continue; // warm-up: allocator arenas settle
        }
        const long long rss = rssKb();
        if (base < 0) {
            base = rss;
        }
        peak_after_base = peak_after_base > rss ? peak_after_base : rss;
    }

    g_stop = true;
    for (auto& w : workers) {
        w.join();
    }
    sig.stop = true;
    runner.join();

    std::error_code ec;
    fs::remove_all(root, ec);

    // Half-open/aborted connections are expected outcomes, counted, not bugs.
    MESSAGE("iterations=" << g_completed.load() << " errors=" << g_errors.load()
                          << " base_kb=" << base << " peak_kb=" << peak_after_base);
    CHECK(g_completed.load() > 240); // 24 workers * 10 s minimum churn
    REQUIRE(peak_after_base > 0);
    CHECK(peak_after_base - base < 16384); // < 16 MiB drift over the soak
}

#else

TEST_CASE("soak is Linux-only (/proc RSS)") {
    MESSAGE("skipped on this platform");
}

#endif
