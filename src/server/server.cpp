#include "server/server.h"

#include "net/error.h"
#include "net/poll.h"
#include "util/time.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace server {

Server::Server(std::uint16_t port, Handler handler) : handler_(std::move(handler)) {
    listener_ = net::Socket::tcp4();
    bool ok = listener_.valid();
    if (ok) {
        ok = listener_.setReuseAddress();
    }
    if (ok) {
        ok = listener_.bindAny(port);
    }
    if (ok) {
        ok = listener_.setNonBlocking(true); // accepted sockets inherit (both OSes)
    }
    if (ok) {
        ok = listener_.listen(128);
    }
    if (!ok) {
        const auto e = net::lastError();
        throw std::runtime_error("listener setup failed (os error " + std::to_string(e.code) + ")");
    }
}

void Server::run(ShutdownSignal& signal) {
    net::PollSet set;
    bool stopping = false;
    std::uint64_t hard = 0;
    for (;;) {
        if (!stopping && signal.stopRequested()) {
            stopping = true;
            listener_.close();
            hard = util::monotonicMs() + static_cast<std::uint64_t>(shutdown_grace.count());
            for (auto& c : conns_) {
                c->beginShutdown();
            }
        }
        if (stopping && (conns_.empty() || util::monotonicMs() >= hard)) {
            break;
        }

        set.clear();
        if (!stopping) {
            set.add(listener_.handle(), net::PollIn);
        }
        signal.preparePoll(set);
        const std::size_t base = set.size(); // connection i ↔ index base + i
        for (auto& c : conns_) {
            set.add(c->socket().handle(),
                    net::PollIn | (c->wantsWrite() ? static_cast<short>(net::PollOut)
                                                   : static_cast<short>(0)));
        }

        const std::uint64_t now = util::monotonicMs();
        int timeout = -1; // -1: block until event (Linux self-pipe wakes on signal)
        for (auto& c : conns_) {
            if (c->dead()) {
                continue;
            }
            const long long left =
                c->deadlineMs() >= now ? static_cast<long long>(c->deadlineMs() - now) : 0;
            timeout =
                timeout < 0 ? static_cast<int>(left) : std::min(timeout, static_cast<int>(left));
        }
        if (const int cap = static_cast<int>(signal.pollCap().count()); cap > 0) {
            timeout = timeout < 0 ? cap : std::min(timeout, cap);
        }
        if (stopping) {
            const long long left = hard >= now ? static_cast<long long>(hard - now) : 0;
            timeout =
                timeout < 0 ? static_cast<int>(left) : std::min(timeout, static_cast<int>(left));
        }

        const int r = net::pollSockets(set, timeout);
        if (r < 0) {
            if (net::lastError().kind == net::ErrorKind::Interrupted) {
                continue; // EINTR: rerun the wait (state may have changed)
            }
            throw std::runtime_error("poll failed");
        }
        signal.afterPoll(set);
        if (!stopping && signal.stopRequested()) {
            continue; // stop detected mid-poll: next iteration shuts down
        }

        const std::size_t existing = conns_.size(); // conns added this round join next poll
        if (!stopping && (set.revents(0) & net::PollIn)) {
            for (int i = 0; i < accept_batch; ++i) {
                net::Socket s = listener_.accept();
                if (!s.valid()) {
                    break; // none pending (or accept error — retried next wakeup)
                }
                conns_.push_back(std::make_unique<Connection>(std::move(s), handler_, idle_timeout,
                                                              max_queued_bytes));
            }
        }

        for (std::size_t i = 0; i < existing; ++i) {
            Connection& c = *conns_[i];
            if (c.dead()) {
                continue;
            }
            const short re = set.revents(base + i);
            if (re & net::PollIn) {
                c.onReadable();
            }
            if (!c.dead() && (re & net::PollOut) && c.wantsWrite()) {
                c.onWritable();
            }
            if (!c.dead() && (re & (net::PollErr | net::PollHup | net::PollNval))) {
                c.onPeerError();
            }
        }

        const std::uint64_t after = util::monotonicMs();
        for (auto& c : conns_) {
            if (!c->dead() && after >= c->deadlineMs()) {
                c->onTimeout();
            }
        }
        reap();
    }
    conns_.clear(); // grace expired with bytes still queued — drop
    signal.notifyStopped();
}

void Server::reap() {
    conns_.erase(std::remove_if(conns_.begin(), conns_.end(),
                                [](const std::unique_ptr<Connection>& c) { return c->dead(); }),
                 conns_.end());
}

} // namespace server
