#include "server/server.h"

#include "net/error.h"
#include "net/poll.h"
#include "net/wakeup.h"
#include "server/connection.h"
#include "util/time.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace server {

Server::Server(std::uint16_t port, Handler handler) : handler_(std::move(handler)) {
    listener_ = net::Socket::tcp4();
    if (!listener_.valid()) {
        throw std::runtime_error("socket creation failed (os error " +
                                 std::to_string(net::lastError().code) + ")");
    }
    if (!listener_.setReuseAddress() || !listener_.setNonBlocking(true) ||
        !listener_.bindAny(port) || !listener_.listen(128)) {
        throw std::runtime_error("listener setup failed (os error " +
                                 std::to_string(net::lastError().code) + ")");
    }
}

// One reactor thread: inbox + its connections. Everything but the inbox is
// touched only by the reactor thread; Connection has no locking because after
// hand-off exactly one thread owns it.
class Server::Reactor {
public:
    explicit Reactor(const Server& owner) : owner_(owner), wake_(net::Wakeup::create()) {}

    bool valid() const { return wake_.valid(); }

    void post(net::Socket socket) {
        {
            const std::lock_guard<std::mutex> g(mu_);
            inbound_.push_back(std::move(socket));
        }
        wake_.notify();
    }

    void requestStop() {
        {
            const std::lock_guard<std::mutex> g(mu_);
            stop_ = true;
        }
        wake_.notify();
    }

    void run();

private:
    void reap();

    const Server& owner_;
    net::Wakeup wake_;
    std::mutex mu_;
    std::vector<net::Socket> inbound_;               // accepted, not yet owned by this reactor
    bool stop_ = false;                              // both under mu_
    std::vector<std::unique_ptr<Connection>> conns_; // reactor-thread-only
};

void Server::Reactor::reap() {
    conns_.erase(std::remove_if(conns_.begin(), conns_.end(),
                                [](const std::unique_ptr<Connection>& c) { return c->dead(); }),
                 conns_.end());
}

void Server::Reactor::run() {
    net::PollSet set;
    bool stopping = false;
    std::uint64_t hard = 0;
    for (;;) {
        if (stopping && util::monotonicMs() >= hard) {
            break; // grace expired with bytes still queued — drop
        }

        set.clear();
        set.add(wake_.readHandle(), net::PollIn);
        for (auto& c : conns_) {
            set.add(c->socket().handle(),
                    net::PollIn | (c->wantsWrite() ? static_cast<short>(net::PollOut)
                                                   : static_cast<short>(0)));
        }
        const std::size_t base = set.size(); // inbox merges append; they join next round

        const std::uint64_t now = util::monotonicMs();
        int timeout = -1; // wake socket makes hand-offs and stop instant; no cap needed
        for (auto& c : conns_) {
            if (c->dead()) {
                continue;
            }
            const long long left =
                c->deadlineMs() >= now ? static_cast<long long>(c->deadlineMs() - now) : 0;
            timeout =
                timeout < 0 ? static_cast<int>(left) : std::min(timeout, static_cast<int>(left));
        }
        if (stopping) {
            const long long left = hard >= now ? static_cast<long long>(hard - now) : 0;
            timeout =
                timeout < 0 ? static_cast<int>(left) : std::min(timeout, static_cast<int>(left));
        }

        const int r = net::pollSockets(set, timeout);
        if (r < 0) {
            if (net::lastError().kind == net::ErrorKind::Interrupted) {
                continue; // EINTR: rerun the wait
            }
            throw std::runtime_error("poll failed");
        }

        std::vector<net::Socket> fresh;
        bool stop_now = false;
        {
            const std::lock_guard<std::mutex> g(mu_);
            fresh.swap(inbound_);
            stop_now = stop_;
        }
        wake_.drain();
        if (!fresh.empty()) {
            conns_.reserve(conns_.size() + fresh.size());
            for (auto& s : fresh) {
                conns_.push_back(std::make_unique<Connection>(
                    std::move(s), owner_.handler_, owner_.idle_timeout, owner_.max_queued_bytes));
            }
        }
        if (stop_now && !stopping) {
            stopping = true;
            for (auto& c : conns_) {
                c->beginShutdown();
            }
            hard = util::monotonicMs() + static_cast<std::uint64_t>(owner_.shutdown_grace.count());
        }
        if (stopping && conns_.empty()) {
            break;
        }

        // Inbox merges joined after this poll round; they get events next round.
        // base-1 = connections present in `set`.
        for (std::size_t i = 1; i < base; ++i) {
            Connection& c = *conns_[i - 1];
            if (c.dead()) {
                continue;
            }
            const short re = set.revents(i);
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
    conns_.clear();
}

void Server::run(ShutdownSignal& signal) {
    unsigned k = threads;
    if (k == 0) {
        k = std::thread::hardware_concurrency();
        if (k == 0) {
            k = 1;
        }
    }

    std::vector<std::unique_ptr<Reactor>> reactors;
    std::vector<std::thread> pool;
    const auto stopAll = [&] {
        for (auto& r : reactors) {
            r->requestStop();
        }
        for (auto& t : pool) {
            if (t.joinable()) {
                t.join();
            }
        }
    };
    try {
        for (unsigned i = 0; i < k; ++i) {
            auto reactor = std::make_unique<Reactor>(*this);
            if (!reactor->valid()) {
                throw std::runtime_error("wake-up socketpair failed");
            }
            Reactor* raw = reactor.get();
            reactors.push_back(std::move(reactor));
            pool.emplace_back([raw] { raw->run(); });
        }
    } catch (...) {
        for (auto& r : reactors) {
            r->requestStop();
        }
        for (auto& t : pool) {
            t.join();
        }
        throw;
    }

    net::PollSet set;
    std::size_t next = 0;
    while (!signal.stopRequested()) {
        set.clear();
        set.add(listener_.handle(), net::PollIn);
        signal.preparePoll(set);

        const int cap = static_cast<int>(signal.pollCap().count());
        const int r = net::pollSockets(set, cap > 0 ? cap : -1);
        if (r < 0) {
            if (net::lastError().kind == net::ErrorKind::Interrupted) {
                continue; // signal (e.g. SIGINT): re-check stop at the loop top
            }
            stopAll();
            throw std::runtime_error("accept poll failed");
        }
        signal.afterPoll(set);
        if (signal.stopRequested()) {
            continue;
        }

        if (set.revents(0) & net::PollIn) {
            for (int i = 0; i < accept_batch; ++i) {
                net::Socket s = listener_.accept();
                if (!s.valid()) {
                    break; // none pending (or accept error — retried next wakeup)
                }
                reactors[next++ % reactors.size()]->post(std::move(s));
            }
        }
    }

    listener_.close();
    stopAll();
    signal.notifyStopped();
}

} // namespace server
