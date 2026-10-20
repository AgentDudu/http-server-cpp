#pragma once

#include <chrono>

namespace net {
class PollSet;
}

namespace server {

// Pluggable stop trigger for the reactor. app/ implements it over OS signals;
// tests drive it directly. The reactor re-checks stopRequested() after every
// poll, so an implementation without a wake handle (Windows console events)
// only needs a bounded pollCap().
class ShutdownSignal {
  public:
    virtual ~ShutdownSignal() = default;

    virtual bool stopRequested() const = 0;

    // Integrate the wake handle into the poll set (Linux self-pipe). Default: none.
    virtual void preparePoll(net::PollSet& set) { (void)set; }

    // Inspect revents after poll; consume a wake and record it as a stop request.
    virtual void afterPoll(const net::PollSet& set) { (void)set; }

    // Maximum poll wait while waiting for the stop signal; 0 = no cap.
    virtual std::chrono::milliseconds pollCap() const { return std::chrono::milliseconds(0); }

    // Called once after the loop ends, before run() returns (lets a blocking
    // Windows console-control handler return).
    virtual void notifyStopped() {}
};

} // namespace server
