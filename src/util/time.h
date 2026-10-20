#pragma once

#include <chrono>
#include <cstdint>

namespace util {

// Wall-clock-independent milliseconds for deadlines (immune to clock changes).
inline std::uint64_t monotonicMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

} // namespace util
