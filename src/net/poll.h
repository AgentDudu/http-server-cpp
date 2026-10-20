#pragma once

#include "net/socket.h"

#include <cstddef>
#include <vector>

namespace net {

// Thin WSAPoll fd set. Only the first pollSockets() return value of entries carry
// meaningful revents (Windows leaves the rest untouched).
class PollSet {
  public:
    void add(SOCKET socket, short events) { fds_.push_back({socket, events, 0}); }
    void clear() { fds_.clear(); }
    std::size_t size() const { return fds_.size(); }
    SOCKET socket(std::size_t index) const { return fds_[index].fd; }
    short revents(std::size_t index) const { return fds_[index].revents; }
    WSAPOLLFD* data() { return fds_.data(); }

  private:
    std::vector<WSAPOLLFD> fds_;
};

// >0 ready entries, 0 timeout, -1 error.
inline int pollSockets(PollSet& set, int timeoutMs) {
    if (set.size() == 0) {
        return 0;
    }
    return ::WSAPoll(set.data(), static_cast<ULONG>(set.size()), timeoutMs);
}

} // namespace net
