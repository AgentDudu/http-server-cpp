#include "net/context.h"

namespace net {

// POSIX needs no socket-subsystem session; the type keeps call sites uniform.
Context::Context() : ok_(true), version_(0) {}
Context::~Context() = default;

} // namespace net
