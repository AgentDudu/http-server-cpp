#pragma once

#include "server/shutdown.h"

#include <memory>

namespace app {

// Process-level SIGINT/SIGTERM (POSIX) or console-control (Windows) shutdown.
// Installs OS handlers in the constructor — one instance per process.
//
// POSIX: self-pipe — the handler writes a byte, the reactor's poll wakes at once.
// Windows: the handler sets a flag and blocks (a returning handler means "handled,
// exit now" would kill the graceful drain); the reactor notices within pollCap().
class ShutdownSignals final : public server::ShutdownSignal {
public:
    struct Impl; // OS state, defined in the per-OS .cpp

    ShutdownSignals();
    ~ShutdownSignals() override;

    bool stopRequested() const override;
    void preparePoll(net::PollSet& set) override;
    void afterPoll(const net::PollSet& set) override;
    std::chrono::milliseconds pollCap() const override;
    void notifyStopped() override;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace app
