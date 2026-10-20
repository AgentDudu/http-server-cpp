#include "app/signals.h"

#include "net/poll.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>

struct app::ShutdownSignals::Impl {};

namespace {
std::atomic_bool g_stop{false};
HANDLE g_done = nullptr; // set once the reactor loop has ended

BOOL WINAPI onConsoleEvent(DWORD type) {
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT && type != CTRL_CLOSE_EVENT &&
        type != CTRL_LOGOFF_EVENT && type != CTRL_SHUTDOWN_EVENT) {
        return FALSE; // not ours; next handler / default
    }
    g_stop.store(true);
    // Returning TRUE means "handled, continue" — but returning at all races the
    // 5s close grace, so block until the reactor is done and let process exit
    // take us out.
    if (g_done != nullptr) {
        ::WaitForSingleObject(g_done, 4000);
    } else {
        ::Sleep(4000);
    }
    return TRUE;
}
} // namespace

namespace app {

ShutdownSignals::ShutdownSignals() : impl_(std::make_unique<Impl>()) {
    g_done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ::SetConsoleCtrlHandler(&onConsoleEvent, TRUE);
}

ShutdownSignals::~ShutdownSignals() {
    ::SetConsoleCtrlHandler(&onConsoleEvent, FALSE);
    if (g_done != nullptr) {
        ::CloseHandle(g_done);
        g_done = nullptr;
    }
}

bool ShutdownSignals::stopRequested() const {
    return g_stop.load();
}

void ShutdownSignals::preparePoll(net::PollSet& set) {
    (void)set;
}
void ShutdownSignals::afterPoll(const net::PollSet& set) {
    (void)set;
}

std::chrono::milliseconds ShutdownSignals::pollCap() const {
    return std::chrono::milliseconds(100); // no waitable socket+event union on WSAPoll
}

void ShutdownSignals::notifyStopped() {
    if (g_done != nullptr) {
        ::SetEvent(g_done);
    }
}

} // namespace app
