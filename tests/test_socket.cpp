#include "doctest.h"

#include "net/poll.h"
#include "net/socket.h"
#include "net/wsa.h"

#include <cstring>
#include <string>

namespace {

sockaddr_in loopback(unsigned short port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7F000001);
    addr.sin_port = htons(port);
    return addr;
}

// Reads until expected byte count or clean EOF; returns bytes collected.
std::string readUntil(net::Socket& sock, std::size_t expected) {
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

TEST_CASE("winsock session negotiates 2.2") {
    net::WsaContext wsa;
    REQUIRE(wsa.ok());
    CHECK(wsa.version() == 0x0202);
}

TEST_CASE("listener accepts a loopback connection and echoes bytes") {
    net::WsaContext wsa;
    REQUIRE(wsa.ok());

    auto listener = net::Socket::tcp4();
    REQUIRE(listener.valid());
    REQUIRE(listener.setReuseAddress());
    REQUIRE(listener.bindAny(0));
    REQUIRE(listener.listen(4));
    REQUIRE(listener.setNonBlocking(true));
    const unsigned short port = listener.localPort();
    REQUIRE(port != 0);

    auto client = net::Socket::tcp4();
    REQUIRE(client.valid());
    REQUIRE(client.connect(loopback(port)));

    net::PollSet set;
    set.add(listener.handle(), POLLIN);
    REQUIRE(net::pollSockets(set, 2000) == 1);

    auto serverSide = listener.accept();
    REQUIRE(serverSide.valid());
    // Accepted sockets inherit the listener's non-blocking mode on Windows.
    REQUIRE(serverSide.setNonBlocking(false));

    const char* msg = "ping";
    REQUIRE(client.sendAll(msg, 4));

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

TEST_CASE("non-blocking accept reports WSAEWOULDBLOCK while idle") {
    net::WsaContext wsa;
    REQUIRE(wsa.ok());

    auto listener = net::Socket::tcp4();
    REQUIRE(listener.valid());
    REQUIRE(listener.bindAny(0));
    REQUIRE(listener.listen(1));
    REQUIRE(listener.setNonBlocking(true));

    net::PollSet set;
    set.add(listener.handle(), POLLIN);
    CHECK(net::pollSockets(set, 10) == 0);

    auto pending = listener.accept();
    CHECK_FALSE(pending.valid());
    CHECK(WSAGetLastError() == WSAEWOULDBLOCK);
}

TEST_CASE("poll set keeps entries until cleared") {
    net::WsaContext wsa;
    REQUIRE(wsa.ok());

    auto a = net::Socket::tcp4();
    auto b = net::Socket::tcp4();
    REQUIRE(a.valid());
    REQUIRE(b.valid());

    net::PollSet set;
    set.add(a.handle(), POLLIN);
    set.add(b.handle(), POLLOUT);
    REQUIRE(set.size() == 2);
    CHECK(set.socket(1) == b.handle());
    set.clear();
    CHECK(set.size() == 0);
    CHECK(net::pollSockets(set, 0) == 0); // empty set never blocks or errors
}
