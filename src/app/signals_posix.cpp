#include "app/signals.h"

#include "net/poll.h"

#include <csignal>
#include <fcntl.h>
#include <poll.h> // for the pollfd revents bits (net/poll.h pulls it too)
#include <unistd.h>

#include <atomic>

struct app::ShutdownSignals::Impl {
    int pipe[2] = {-1, -1};
};

namespace {
app::ShutdownSignals::Impl* g_impl = nullptr;
std::atomic_bool g_stop{false};

// Async-signal-safe: write() only. The atomic store is a fallback for a failed
// self-pipe; atomic_bool is lock-free on every platform we target.
void onSignal(int) {
    g_stop.store(true);
    if (g_impl != nullptr && g_impl->pipe[1] >= 0) {
        const char byte = 'x';
        const ssize_t ignored = ::write(g_impl->pipe[1], &byte, 1);
        (void)ignored;
    }
}
} // namespace

namespace app {

ShutdownSignals::ShutdownSignals() : impl_(std::make_unique<Impl>()) {
    if (::pipe2(impl_->pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        impl_->pipe[0] = impl_->pipe[1] = -1;
        return; // degraded: reactor still re-checks after every poll
    }
    g_impl = impl_.get();
    struct sigaction sa{};
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; // send/poll EINTR handling exists, but avoid churn
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
    ::signal(SIGPIPE, SIG_IGN); // belt over MSG_NOSIGNAL's trousers
}

ShutdownSignals::~ShutdownSignals() {
    if (impl_->pipe[0] >= 0) {
        ::close(impl_->pipe[0]);
        ::close(impl_->pipe[1]);
    }
}

bool ShutdownSignals::stopRequested() const {
    return g_stop.load();
}

void ShutdownSignals::preparePoll(net::PollSet& set) {
    if (impl_->pipe[0] >= 0) {
        set.add(impl_->pipe[0], net::PollIn);
    }
}

void ShutdownSignals::afterPoll(const net::PollSet& set) {
    for (std::size_t i = 0; i < set.size(); ++i) {
        if (set.socket(i) == impl_->pipe[0] && (set.revents(i) & net::PollIn)) {
            char drain[64];
            while (::read(impl_->pipe[0], drain, sizeof(drain)) > 0) {
            }
            g_stop.store(true);
            return;
        }
    }
}

std::chrono::milliseconds ShutdownSignals::pollCap() const {
    return std::chrono::milliseconds(0); // self-pipe wakes poll immediately
}

void ShutdownSignals::notifyStopped() {}

} // namespace app
