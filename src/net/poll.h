#pragma once

#include "net/detail.h"
#include "net/detail_raw.h"

#include <cstddef>
#include <vector>

namespace net {

// Thin poll()/WSAPoll fd set. Only the first pollSockets() return value of entries
// carry meaningful revents (Windows leaves the rest untouched).
class PollSet {
  public:
    void add(detail::handle_t socket, short events) {
        fds_.push_back({socket, events, 0});
    }
    void clear() { fds_.clear(); }
    std::size_t size() const { return fds_.size(); }
    detail::handle_t socket(std::size_t index) const { return fds_[index].fd; }
    short revents(std::size_t index) const { return fds_[index].revents; }
    detail::pollfd_t* data() { return fds_.data(); }

  private:
    std::vector<detail::pollfd_t> fds_;
};

// >0 ready entries, 0 timeout, -1 error.
inline int pollSockets(PollSet& set, int timeoutMs) {
    if (set.size() == 0) {
        return 0;
    }
    return detail::rawPoll(set.data(), static_cast<unsigned long>(set.size()), timeoutMs);
}

} // namespace net
