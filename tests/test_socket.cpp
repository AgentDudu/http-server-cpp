#include "doctest.h"

#include "net/context.h"
#include "net/error.h"
#include "net/poll.h"
#include "net/socket.h"

#include <cstring>
#include <string>

using net::Context;
using net::ErrorKind;
using net::PollSet;
using net::Socket;

namespace {

sockaddr_in loopback(unsigned short port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7F000001);
    addr.sin_port = htons(port);
    return addr;
}

// Reads until expected byte count or clean EOF; returns bytes collected.
std::string readUntil(Socket& sock, std::size_t expected) {
    std::string out;
    char buf[64];
    while (out.size() < expected) {
        const int n = sock.recvSome(buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
}

} // namespace

TEST_CASE("socket context initialises") {
    Context ctx;
    REQUIRE(ctx.ok());
#ifdef _WIN32
    CHECK(ctx.version() == 0x0202);
#endif
}

TEST_CASE("listener accepts a loopback connection and echoes bytes") {
    Context ctx;
    REQUIRE(ctx.ok());

    auto listener = Socket::tcp4();
    REQUIRE(listener.valid());
    REQUIRE(listener.setReuseAddress());
    REQUIRE(listener.bindAny(0));
    REQUIRE(listener.listen(4));
    REQUIRE(listener.setNonBlocking(true));
    const unsigned short port = listener.localPort();
    REQUIRE(port != 0);

    auto client = Socket::tcp4();
    REQUIRE(client.valid());
    REQUIRE(client.connect(loopback(port)));

    PollSet set;
    set.add(listener.handle(), POLLIN);
    REQUIRE(net::pollSockets(set, 2000) == 1);

    auto serverSide = listener.accept();
    REQUIRE(serverSide.valid());

    // The accepted socket inherited the listener's non-blocking mode on BOTH
    // platforms (Linux does not do this natively); an empty read must report
    // WouldBlock instead of blocking. Probe before the client sends anything.
    char probe[8]{};
    CHECK(serverSide.recvSome(probe, sizeof(probe)) == -1);
    CHECK(net::lastError().kind == ErrorKind::WouldBlock);

    const char* msg = "ping";
    REQUIRE(client.sendAll(msg, 4));

    REQUIRE(serverSide.setNonBlocking(false));
    char buf[16]{};
    const int n = serverSide.recvSome(buf, sizeof(buf));
    REQUIRE(n == 4);
    CHECK(std::memcmp(buf, msg, 4) == 0);

    REQUIRE(serverSide.sendAll(buf, 4));
    CHECK(readUntil(client, 4) == "ping");

    REQUIRE(client.close());
    // Clean EOF: server side observes a zero-length read.
    CHECK(serverSide.recvSome(buf, sizeof(buf)) == 0);
    serverSide.close();
    listener.close();
}

TEST_CASE("non-blocking accept reports WouldBlock while idle") {
    Context ctx;
    REQUIRE(ctx.ok());

    auto listener = Socket::tcp4();
    REQUIRE(listener.valid());
    REQUIRE(listener.bindAny(0));
    REQUIRE(listener.listen(1));
    REQUIRE(listener.setNonBlocking(true));

    PollSet set;
    set.add(listener.handle(), POLLIN);
    CHECK(net::pollSockets(set, 10) == 0);

    auto pending = listener.accept();
    CHECK_FALSE(pending.valid());
    CHECK(net::lastError().kind == ErrorKind::WouldBlock);
}

TEST_CASE("poll set keeps entries until cleared") {
    Context ctx;
    REQUIRE(ctx.ok());

    auto a = Socket::tcp4();
    auto b = Socket::tcp4();
    REQUIRE(a.valid());
    REQUIRE(b.valid());

    PollSet set;
    set.add(a.handle(), POLLIN);
    set.add(b.handle(), POLLOUT);
    REQUIRE(set.size() == 2);
    CHECK(set.socket(1) == b.handle());
    set.clear();
    CHECK(set.size() == 0);
    CHECK(net::pollSockets(set, 0) == 0); // empty set never blocks or errors
}
