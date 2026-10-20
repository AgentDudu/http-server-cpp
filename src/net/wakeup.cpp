#include "net/wakeup.h"

#include "net/detail_raw.h"

#include <cstdint>

namespace net {

Wakeup Wakeup::create() {
    detail::handle_t pair[2] = {detail::kInvalidHandle, detail::kInvalidHandle};
    if (!detail::rawSocketPair(pair)) {
        return Wakeup{};
    }
    return Wakeup{Socket{pair[0]}, Socket{pair[1]}};
}

void Wakeup::notify() {
    if (!out_.valid()) {
        return;
    }
    const char byte = 'w';
    out_.sendSome(&byte, 1); // WouldBlock simply means a wake-up is already pending
}

void Wakeup::drain() {
    if (!in_.valid()) {
        return;
    }
    char buf[64];
    while (in_.recvSome(buf, sizeof(buf)) == static_cast<int>(sizeof(buf))) {
    }
}

} // namespace net
