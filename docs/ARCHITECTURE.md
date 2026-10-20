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
- `http/` — status, headers, request parser, response serialization landed. Parser is
  push/incremental, enforces head/body/count limits, and rejects bare-LF endings,
  obsolete folding, duplicate `Content-Length`, `Content-Length`+`Transfer-Encoding`,
  non-token names/methods, unknown versions. URL decode/normalise and MIME live
  with the static handler (they are a serving concern, not a parsing one).
- `server/` — acceptor thread + K reactor threads (`Server::run`, `threads=0` →
  hardware_concurrency). The acceptor batches accepts and hands each socket to a
  reactor round-robin over a mutex-guarded inbox plus a `net::Wakeup` socketpair;
  a `Connection` (socket + `RequestParser` + capped output buffer
  `max_queued_bytes`) is owned by exactly one reactor forever — no connection
  state is ever shared or locked. Poll timeouts come from per-connection
  deadlines (408); graceful stop drains queued responses per reactor, grace cap,
  then drops. EINTR from poll is just a re-check (the stop path is the only
  joiner). A slow reader throttles its connection — queued responses stop
  growing, remaining pipelined requests wait in the parser, POLLOUT or the next
  read resumes the drain.
  Handlers return `Reply{status, content_type, body}`; HEAD keeps an honest
  `Content-Length` and no body; handler exceptions become 500 at the reactor seam.
  `StaticFiles` serves a canonical root: strict percent-decode before resolution,
  traversal/encoded/backslash/absolute targets refused as 404 before touching disk
  (symlink prefixes resolved via `weakly_canonical`, case-folded containment on
  Windows), index.html for directories, 405 elsewhere, whole-file reads.
  Protocol errors are answered once (best effort) then the connection drops —
  smuggling candidates never share a stream. `ShutdownSignal` is the pluggable stop
  trigger; tests drive it directly.
- `app/` — shutdown signals, one paragraph precisely because behaviour differs:
  Linux uses a self-pipe (`pipe2`, handler writes one byte → poll wakes instantly,
  SIGINT drains and exits 0 — smoke-tested in the container gate); Windows cannot
  union a waitable event with WSAPoll, so the console-control handler sets a flag
  and blocks until the loop ends (`notifyStopped()`), with the reactor capped at a
  100 ms poll while idle. A returning handler would mean "exit now" — hence block.

Gates: `tools/ck.ps1` (Windows), `tools/ck-asan-docker.sh` (whole tree, ASan+UBSan,
Ubuntu 24.04 container, plus SIGINT graceful-stop smoke), and TSan (`SAN=thread`,
Linux runner — kernel containers can't set the `vm.mmap_rnd_bits` TSan needs).
The socket and server test files are the same source on both platforms.
