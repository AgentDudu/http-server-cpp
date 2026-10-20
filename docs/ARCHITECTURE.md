# Architecture

Subsystem docs land with the code; PLAN.md sections 3-4 describe the intended design
until then.

- `net/` — the platform seam. Everything above it is single-source, no OS macros.
  - Portable API: `Socket` (move-only handle), `PollSet`/`pollSockets()`, `Context`,
    `Error`/`lastError()`. `detail.h` exposes `handle_t`/`pollfd_t`; `detail_raw.h`
    declares the OS primitives; `socket_common.cpp` holds the shared logic.
  - Backends: `raw_win.cpp`/`error_win.cpp`/`context_win.cpp` (Winsock) and
    `raw_posix.cpp`/`error_posix.cpp`/`context_posix.cpp` (BSD sockets). CMake picks
    the directory by platform.
  - Unified contract that costs the most care: **an accepted socket's blocking mode
    matches the listener's** — Winsock inherits automatically, Linux does not
    (`accept4(SOCK_CLOEXEC)` + explicit `O_NONBLOCK` copy).
  - Linux sends with `MSG_NOSIGNAL` (no `SIGPIPE` deaths); `EINTR` retried in
    send/recv, reported to the reactor from poll as `ErrorKind::Interrupted`.
  - `glibc` needs `_GNU_SOURCE` with `-std=c++20` for `accept4`/`SOCK_CLOEXEC`.
  - glibc `setsockopt` requires `int` option size; Winsock is lenient.
- `http/` — status, headers, response serialization landed (strict head: CTL/CR/LF/NUL
  rejected, implied Content-Length, chunk encoder). Pending: request parser, URL
  decode/normalise, MIME.
- `app/` — signal handling (pending, win + posix)
- `server/` — listener, connection, static file handler (pending)

Gates: `tools/ck.ps1` (Windows) and `tools/ck-asan-docker.sh` (whole tree, ASan+UBSan,
Ubuntu 24.04 container). The socket test file is the same source on both platforms.
