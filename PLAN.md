# http-server-cpp — Build Plan (v2, dual-platform)

Small, clean, single-binary HTTP/1.1 file server in C++20. Runs natively on Windows and
Linux. No network library in the shipped binary. Portfolio artifact: readability and
correctness are the deliverables, raw throughput is not.

v2 change: Linux became a first-class target. The split happens **before the reactor
exists** — reactor, signals, shutdown, file serving, and all their tests are written
cross-platform from their first commit.

---

## 1. Locked decisions

| Topic | Decision | Why |
|---|---|---|
| Goal | Portfolio artifact, small and clean | Optimise for readable code + honest tests, not feature count |
| HTTP | HTTP/1.1 server core: methods, status codes, chunked, keep-alive, pipelining | Contract with every real client |
| Content | Static files, MIME, index, ETag/`If-Modified-Since`, `Range` | Makes it actually usable |
| Platforms | Windows (native, MinGW-w64) **and** Linux (GCC in Docker) | v2; both built, tested, and sanitised on every PR |
| I/O | Non-blocking sockets, **`poll()` / `WSAPoll()` everywhere**, K reactor threads, per-connection timeouts, graceful shutdown | Same `pollfd` semantics on both; reactor stays single-source |
| Event engines | No epoll/io_uring/IOCP backend abstraction | poll is right-sized for a file server; engines need a benchmark to justify them, and we have none |
| Standard | C++20 | Full support on both GCC 15.2 (MinGW) and Ubuntu 24.04's GCC 13 |
| Deps | doctest (tests only, vendored). Server binary: std + platform socket API only | Zero third-party code shipped |
| Config | CLI flags: `--port --root --threads --max-body --timeout --log-level` | A config file is one more parser to justify |
| Linux toolchain | Docker-only on this host (`http-cpp-asan` image: Ubuntu 24.04, g++, cmake, ninja) | No WSL distro; CI mirrors the container exactly |
| CI | GitHub Actions matrix `ubuntu-latest` + `windows-latest` on every PR to `develop`; ASan native on Linux; TSan once the reactor lands | Linux CI turns the fake sanitizer gates real |

## 2. Explicit non-goals

- HTTP/2 and HTTP/3 — separate binary framing, HPACK, multiplexing; different project.
- TLS / HTTPS — needs a crypto dependency; violates the deps policy.
- CGI, subprocess execution, embedded route handlers.
- WebSocket upgrade.
- macOS/kqueue (poll() portability means it likely works; we will not claim or test it).
- epoll/io_uring/IOCP event engines — revisit only with benchmark evidence.
- IPv6. IPv4 first; documented as a limitation.
- Daemon/service mode, Windows event log, systemd unit files. stderr logging with levels.

## 3. Architecture

One codebase, one seam. Everything above `net/` never sees a platform macro.

```
                     ┌──────────────────────────────┐
  client ── TCP ──►  │  listener (non-blocking)     │
                     └──────────────┬───────────────┘
                                    │ round-robin hand-off (mutex-protected inbox;
              ┌─────────────────┬───┴────────────────┬─────────────┐   TSan-proven in CI)
              ▼                 ▼                    ▼
      ┌───────────────┐  ┌───────────────┐   ┌───────────────┐
      │ reactor 0     │  │ reactor 1     │ … │ reactor K-1   │
      │ poll loop     │  │ poll loop     │   │ poll loop     │
      │ conns owned   │  │ conns owned   │   │ conns owned   │
      └───────┬───────┘  └───────────────┘   └───────────────┘
              ▼
      Connection state machine ──► http::Request ──► StaticFileHandler ──► http::Response

  src/http, src/server, reactor, connection  ─── single source
  src/net:  socket.h poll.h error.h (portable API)
            win/ (winsock impl)      posix/ (BSD socket impl)   ← the only seam
```

**Ownership rule.** A `Connection` is created by exactly one reactor and destroyed by
that same reactor. The one shared structure is the per-reactor hand-off inbox, guarded
by a short-held mutex — proof is TSan in Linux CI, not vibes.

**Reactor loop (per thread, single source).**
1. `poll()` / `WSAPoll()` with timeout = smallest remaining deadline.
2. Ready listener → accept loop (non-blocking), hand `Connection` to next reactor inbox.
3. Ready connection → drive state machine: `RECV` → `PARSE` → `SERVE` → `SEND` →
   `KEEP_ALIVE` or `CLOSE`.
4. Expired deadline (idle / slow-body / send) → `408` or `400` then close.
5. Shutdown flag set → stop accepting, drain or timeout in-flight, join, cleanup.

**Connection state machine**

```
   ACCEPTED → READING_HEADS ──bad──► RESPOND_400 → CLOSING
                 │ complete
                 ▼
              SERVE  (map path → file, stat, etag/304, range/206)
                 │
                 ▼
              SENDING ──WOULDBLOCK──► (poll POLLOUT) ──► SENDING
                 │ done                keep-alive? ──yes──► READING_HEADS
                 ▼ no
              CLOSING (graceful: flush, shutdown(SD_SEND), close)
```

**Failure policy.** Every socket error closes the connection, never the process. Error
codes cross the seam as `net::Error { WouldBlock | Interrupted | Closed | Other, code }`
mapped from `WSAGetLastError()`/`errno` inside `net/`. No exceptions cross the reactor
boundary; exceptions confined to setup.

### 3.1 Platform traps — planned, not discovered

| Trap | Windows | Linux |
|---|---|---|
| Accepted socket blocking mode | **inherits** listener's non-blocking | **does not inherit** → `accept4(SOCK_NONBLOCK \| SOCK_CLOEXEC)`, fallback `fcntl` |
| Send to dead peer | `send` → `WSAECONNRESET`, no signal | `SIGPIPE` kills process → `MSG_NOSIGNAL` on every send |
| Interrupted calls | effectively none | `EINTR` on `poll`/`read`/`write` → retry loop |
| Process shutdown | `SetConsoleCtrlHandler`, poll-timeout-bounded | `SIGINT`/`SIGTERM` → self-pipe wakes poll |
| Session lifecycle | `WSAStartup`/`WSACleanup` in `net::Context` | no-op in `net::Context` |
| Address reuse | `SO_REUSEADDR` | `SO_REUSEADDR` (no `SO_EXCLUSIVEADDRUSE` analogue needed) |
| `select` legacy ceiling | n/a (`WSAPoll` used) | n/a (`poll()` used) |

## 4. Layout

```
CMakeLists.txt            CMakePresets.json (Windows; Linux configures inside container)
cmake/
.github/workflows/ci.yml  matrix: ubuntu-latest, windows-latest + sanitizers
src/
  main.cpp                flags, logger, start/stop
  app/    signals_win.cpp / signals_posix.cpp   → sets shutdown flag, wakes reactor
  net/    socket.h poll.h error.h context.h     portable API, documented above
          win/   socket.cpp wsa.cpp poll.cpp
          posix/ socket.cpp context.cpp poll.cpp
  http/   request.* response.* headers.* mime.* status.* url.*   (response/headers/status landed)
  server/ server.* static_file.*               single source, std::filesystem
include/                  (none unless a public lib target is ever wanted)
tests/                    doctest: unit + integration, cross-platform
tests/corpus/             parser test vectors (valid + malformed)
tools/ck.ps1              Windows gate: build + test + format + staged-secrets guard
tools/ck-asan-docker.sh   Linux gate: full tree (net+http+server) ASan+UBSan
tools/ck-tsan-docker.sh   Linux gate: TSan on threaded reactor tests (once reactor lands)
tools/Dockerfile.asan     Ubuntu 24.04 + g++ + cmake + ninja
docs/ARCHITECTURE.md
```

## 5. Build, run, gates

Windows: presets `gcc-debug`, `gcc-release` (MinGW pinned absolutely, static runtime —
PATH-proof binaries). Linux: configure/build inside the `http-cpp-asan` image; no host
toolchain. `ctest` on both. CI matrix runs both plus ASan (and TSan post-reactor).

Flags both platforms: `-std=c++20 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`.
Windows additionally: `_WIN32_WINNT=0x0A00`, link `ws2_32`, `-static`.

**Sanitizer reality (verified).** MinGW-w64 here has no `libasan`/`libubsan` and no TSan
on Windows at all. Therefore: ASan+UBSan (leak check on) **native on Linux CI** for the
whole tree; TSan **Linux CI** for reactor/concurrency tests. `gcc-asan` Windows preset
self-detects and fails with guidance; `ck.ps1` warns and skips. `tools/ck-asan-docker.sh`
verified green on `http/`.

## 6. Testing strategy

| Layer | Where | Covers |
|---|---|---|
| Unit | both OSes | parser (byte-exact state transitions), headers, URL decode/normalise, MIME, Range, chunked, timeout math |
| Corpus | both OSes + ASan | malformed / request-smuggling vectors; deterministic replay replaces a fuzzer |
| Socket | both OSes (once cross-platform) | bind/accept/echo/WOULDBLOCK/EOF — same test file compiled twice |
| Integration | both OSes | request→file→response, 206, 304, 416, 400, 405, 408, keep-alive, pipelining, truncated body, oversize heads |
| Concurrency | Linux + TSan | N clients × keep-alive × K reactors × hand-off inbox; no race, no leak, correct counts |
| Load | Linux (and Windows manually) | sanity req/s, fd leak, RSS flat |

Parser invariants asserted everywhere: never reads past declared body, header count and
sizes bounded, rejects bare-LF folding, duplicate `Content-Length`, and
`Content-Length` + `Transfer-Encoding` together.

## 7. Milestones

Each milestone = one `feature/*` branch → PR → `develop`. Exit gate: CI green on both
OSes + the listed behaviour proven by a test. Commit messages: intent only, no
phase/step/milestone wording, ever.

| # | Branch | Delivers | Exit criterion |
|---|---|---|---|
| ✓ | ~~`feature/build-scaffold`~~ | CMake, presets, doctest, `ck.ps1` | merged |
| ✓ | ~~`feature/socket-layer`~~ | Winsock RAII, non-blocking, WSAPoll | merged |
| ✓ | ~~`feature/response-writer`~~ | Response, headers, status, chunk encoder | merged |
| P | `feature/posix-socket-layer` | The seam: `socket.h/poll.h/error.h/context.h` portable API; `net/win/` + `net/posix/` split; `accept4` + `MSG_NOSIGNAL` + `EINTR` discipline on Linux; `test_socket.cpp` cross-platform; Docker gate builds whole Linux tree | same socket test file green on Windows **and** in container (ASan) |
| C | `feature/ci-matrix` | GitHub Actions `ubuntu + windows` matrix, ASan job on Linux, `ck.ps1`/container parity with CI | PR #next is gated by it |
| R | `feature/reactor-and-connection` | poll loop, connection state machine, hand-off inbox, timeouts, buffering, signals (win + posix), graceful shutdown — all single-source except `app/signals_*` | integration: in-memory 200, keep-alive, 408 idle, Ctrl-C/SIGTERM exit 0; **TSan job added and green** |
| S | `feature/static-files` | root mapping, traversal guard, MIME, index, 404/405, error pages (`std::filesystem`) | status-by-status integration tests; traversal rejected on both OSes |
| G | `feature/caching-and-range` | ETag, Last-Modified, If-None-Match, Range/206/416 | 304 + 206 tests incl. multi-range rejection |
| H | `feature/hardening` | pipelining, slow-body limit, max-body/max-heads, request-smuggling corpus, soak | pipelined + slow-client tests; ASan clean; 60 s soak flat RSS on Linux |
| D | `feature/docs-and-benchmark` | README (build/run per OS, limits), load script, numbers | documented; req/s + RSS recorded on both platforms |

Then `release/0.1.0` → `develop` + PR to `main` (approval required) → tag `v0.1.0`.
P and C may proceed in either order; R needs both.

## 8. Git workflow (gitflow)

- `main` — releases only. **I open a PR and stop; merge waits for your approval.**
  `main` is currently held/absent by explicit decision.
- `develop` — integration. I push and merge freely here.
- `feature/<topic>` → `develop`, rebase-merge so develop stays linear.
- `release/<x.y.z>` → `develop` + `main` with `--no-ff`; tag `vx.y.z`.
- `hotfix/<name>` → `main` + `develop`; PR to `main` still needs your approval.
- Conventional Commits, intent only — never "phase/step/milestone" wording.
- One logical change per commit; every commit builds on both OSes (CI enforces from C).
- **Every PR must be green on both Ubuntu and Windows before merge to `develop`.**

## 9. Security rules for this repo

- No secrets, tokens, or absolute private paths in any tracked file or CI secret-less log.
- No credentials in the remote URL; `gh` credential helper only.
- `.gitignore` blocks build output, `.env`, `*.pem`, `*.key`.
- Server-side security is a feature: path traversal (both separators, dot-segments),
  header smuggling (nothing user-controlled echoed raw), oversized heads/bodies,
  request smuggling (`CL.TE`/`TE.CL` corpus cases), fd exhaustion caps, `MSG_NOSIGNAL`.

## 10. Resolved decisions

1. Remote: `https://github.com/AgentDudu/http-server-cpp.git`, pushed via `gh` CLI
   credential helper — no tokens in remotes, configs, or files.
2. Default `--root ./www` with a bundled sample tree.
3. doctest v2.4.11 vendored in `external/doctest/`.
4. `main` is held; `develop` is the default branch.
5. **v2:** Linux is first-class; port happens now (only `net/` is platform-specific —
   ~250 lines — and nothing above it exists yet). poll() on both OSes; Docker is the
   Linux toolchain; CI matrix with native ASan + TSan replaces the fake gates.
