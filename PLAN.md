# http-server-cpp — Build Plan

Small, clean, single-binary HTTP/1.1 file server in C++20. Windows-native. No network
library in the shipped binary. Portfolio artifact: readability and correctness are the
deliverables, raw throughput is not.

---

## 1. Locked decisions

| Topic | Decision | Why |
|---|---|---|
| Goal | Portfolio artifact, small and clean | Optimise for readable code + honest tests, not feature count |
| HTTP | HTTP/1.1 server core: methods, status codes, chunked, keep-alive, pipelining | Contract with every real client |
| Content | Static files, MIME, index, ETag/`If-Modified-Since`, `Range` | Makes it actually usable |
| I/O | Non-blocking sockets, `WSAPoll`, K reactor threads, per-connection timeouts, graceful shutdown | Windows-native; scales without a thread per client |
| Standard | C++20 | Full support in GCC 13.2; C++23 stdlib is partial on MinGW |
| Compiler | MinGW-w64 GCC 13.2 (`ucrt-posix-seh`) + CMake 3.29 + Ninja | Installed; no MSVC/clang on host |
| Deps | doctest (tests only). Server binary: std + Winsock only | Zero third-party code shipped |
| Config | CLI flags: `--port --root --threads --max-body --timeout --log-level` | A config file is one more parser to justify |

## 2. Explicit non-goals

- HTTP/2 and HTTP/3 — separate binary framing, HPACK, multiplexing; different project.
- TLS / HTTPS — needs a crypto dependency; violates the deps policy.
- CGI, subprocess execution, embedded route handlers.
- WebSocket upgrade.
- Linux/macOS support, and the portable socket-abstraction layer that would imply.
- IOCP. `WSAPoll` meets the concurrency target; IOCP doubles the async surface.
- IPv6. IPv4 first; documented as a limitation.
- Daemon/service mode, Windows event log. stderr logging with levels.

## 3. Architecture

```
                     ┌──────────────────────────────┐
  client ── TCP ──►  │  listener (non-blocking)     │
                     └──────────────┬───────────────┘
                                    │ round-robin hand-off
              ┌─────────────────────┴─────────────────────┐
              ▼                     ▼                     ▼
      ┌───────────────┐     ┌───────────────┐      ┌───────────────┐
      │ reactor 0     │     │ reactor 1     │ ...  │ reactor K-1   │
      │ WSAPoll loop  │     │ WSAPoll loop  │      │ WSAPoll loop  │
      │ conns owned   │     │ conns owned   │      │ conns owned   │
      └───────┬───────┘     └───────────────┘      └───────────────┘
              │ drives
              ▼
      Connection state machine ──► http::Request ──► StaticFileHandler ──► http::Response
        (parse / serve / close)                          │
                                                   FileCache-less: stat + open
                                                   per request, size-capped reads
```

**Ownership rule.** A `Connection` is created by exactly one reactor and destroyed by
that same reactor. Reactors share nothing mutable except the listen socket. This is the
substitute for TSan: the design has no data race to find.

**Reactor loop (per thread).**
1. `WSAPoll(fds, timeout = smallest remaining deadline)`.
2. Ready listener → `accept` (non-blocking, loop on `WOULDBLOCK`), hand `Connection` to
   next reactor.
3. Ready connection → drive state machine: `RECV` → `PARSE` → `SERVE` → `SEND` →
   `KEEP_ALIVE` or `CLOSE`.
4. Expired deadline (idle / slow-body / send) → `408` or `400` then close.
5. Shutdown requested → stop accepting, drain or timeout in-flight, join, `WSACleanup`.

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

**Failure policy.** Every socket error closes the connection, never the process. No
exceptions cross the reactor boundary — error codes at the socket layer, `Result`-style
returns in `http`, exceptions confined to setup.

## 4. Layout

```
CMakeLists.txt            CMakePresets.json
cmake/                    warnings + sanitar presets
src/
  main.cpp                flags, logger, signal handler, start/stop
  net/    socket.h/.cpp   RAII SOCKET, WsaContext, non-blocking, WSAPoll wrapper
          reactor.h/.cpp  poll loop, connection registry, hand-off, shutdown
          connection.*    state machine, buffered read/write, deadlines
  http/   request.*       incremental parser (state machine, no full-buffer copies)
          response.*      status, headers, body/file part, chunked encoder
          headers.*       case-insensitive table
          mime.*          extension → content-type
          status.*        code → reason phrase
          url.*           percent-decode, dot-segment normalisation
  server/ server.*        owns listener + reactors, config → runtime
          static_file.*   root mapping, traversal guard, ETag, Last-Modified, Range
include/                  (only if a public lib target is ever wanted; else none)
tests/                    doctest: unit + integration (loopback, ephemeral port)
tests/corpus/             parser test vectors (valid + malformed)
tools/ck.ps1              build + test + format + sanitize, one command
docs/ARCHITECTURE.md
```

## 5. Build, run, gates

Presets: `gcc-debug`, `gcc-release`, `gcc-asan` (+`ubsan`).

```
cmake --preset gcc-debug  && cmake --build --preset gcc-debug
ctest --preset gcc-debug            # unit + integration
tools/ck.ps1                        # warnings-off, tests, asan run, format check
```

Flags: `-std=c++20 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`,
`_WIN32_WINNT=0x0A00`, link `ws2_32`. Release: `-O2`. ASan build for parser + connection
runs. Formatting: `clang-format`, enforced by `--dry-run -Werror` in `tools/ck.ps1`
(skipped with a visible warning when the tool is absent).

**Sanitizer reality (verified on this host):** MinGW-w64 GCC 13.2 and 15.2 installs both
lack `libasan`/`libubsan`, and GCC has no TSan on Windows at all. `gcc-asan` preset
therefore self-detects and fails configure with a clear message; `ck.ps1` skips the
native ASan gate with a loud warning. Replacement gate: `tools/ck-asan-docker.sh`
builds and runs the platform-neutral unit tests under ASan+UBSan (leak check on) in an
Ubuntu 24.04 container — verified green. Needs Docker Desktop running.

Windows notes that will bite, handled up front:
- `WSAStartup`/`WSACleanup` owned by a single RAII context in `main`.
- No `SIGPIPE` on Windows; `send` returns `WSAECONNRESET`/`WSAENOTCONN` → treat as client
  gone, close quietly.
- No `SIGTERM`; graceful shutdown via `SetConsoleCtrlHandler` (`CTRL_C_EVENT`,
  `CTRL_CLOSE_EVENT`) plus a self-pipe/`PostQueuedCompletionStatus`-free wake: reactor
  poll timeout bounds shutdown latency.
- `closesocket` on a socket blocked in another thread does not reliably unblock — the
  reactor owns its sockets, so shutdown is a flag flip, and `WSAPoll` returns on timeout.
- Console UTF-8 output set once (`SetConsoleOutputCP`).

## 6. Testing strategy

| Layer | Tool | Covers |
|---|---|---|
| Unit | doctest | parser (byte-exact state transitions), header table, URL decode/normalise, MIME, Range parse+merge, chunked encode/decode, timeout math |
| Corpus | doctest over `tests/corpus/*` | malformed/request-smuggle cases; deterministic replay stands in for a fuzzer (libFuzzer absent on MinGW) |
| Integration | doctest + real loopback sockets | full request→file→response, 206, 304, 416, 400, 405, 408, keep-alive reuse, pipelined batch, truncated body, oversize header rejection |
| Concurrency | threads hammering the server | N clients × keep-alive, no leak, no crash, correct counts; ASan build |
| Load | `curl`/`wrk` if available, else small bundled client | sanity req/s, no fd leak, RSS flat |

Parser invariants asserted everywhere: never reads past declared body, header count/size
bounded, total head size bounded, rejects bare LF line folding, rejects duplicate
`Content-Length`, rejects `Content-Length` + `Transfer-Encoding`.

## 7. Milestones

Each milestone = one `feature/*` branch → PR → `develop`. Exit gate: `tools/ck.ps1`
green + the listed behaviour proven by a test. Names describe the capability; commit
messages never reference a phase or step.

| # | Branch | Delivers | Exit criterion |
|---|---|---|---|
| M0 | `feature/build-scaffold` | CMake + presets, warnings-as-errors, doctest vendored, `.gitignore`, `ck.ps1`, `docs/ARCHITECTURE.md` stub | clean build, "0 tests" run passes |
| M1 | `feature/socket-layer` | `WsaContext`, RAII `Socket`, non-blocking, `PollSet`, listener binds | unit test: bind/accept/connect/echo on loopback |
| M2 | `feature/response-writer` | `Response` builder, header serialisation, chunked encoder, status table | byte-exact unit tests incl. CRLF discipline |
| M3 | `feature/request-parser` | incremental `Request` parser, limits, chunked + `Content-Length` decode, corpus suite | all corpus vectors pass; ASan clean on corpus |
| M4 | `feature/reactor-and-connection` | poll loop, connection state machine, hand-off, read/write buffering, idle timeout | integration: serves a fixed 200 from memory, keep-alive works, 408 on idle |
| M5 | `feature/static-files` | root mapping, traversal guard, MIME, index, 404/405, error pages | integration suite for each status; traversal attempts rejected |
| M6 | `feature/caching-and-range` | ETag, `Last-Modified`/`If-Modified-Since`, `If-None-Match`, `Range`/`206`/`416` | 304 and 206 tests incl. multi-range rejection |
| M7 | `feature/hardening` | pipelining, slow-body limit, `max-body`/`max-heads` enforcement, graceful shutdown, signal handler | pipelined + slow-client tests; Ctrl-C exits 0 with all fds closed |
| M8 | `feature/docs-and-benchmark` | README (build/run/limits), load script, RSS/fd soak | documented, numbers recorded, soak 60 s flat RSS |

Then `release/0.1.0` → merge to `develop` + PR to `main` (approval required) → tag `v0.1.0`.

## 8. Git workflow (gitflow)

- `main` — releases only. **I open a PR and stop; merge waits for your approval.**
- `develop` — integration. I push and merge freely here.
- `feature/<topic>` → `develop`, rebase or squash so develop stays linear.
- `release/<x.y.z>` → `develop` + `main` with `--no-ff`; bump version; tag `vx.y.z`.
- `hotfix/<name>` → `main` + `develop`; PR to `main` still needs your approval.
- Conventional Commits: `feat: incremental request parser`, `fix: reject duplicate Content-Length`,
  `test: cover multi-range rejection`, `docs: record load-test numbers`.
  Intent only — **no phase/step/milestone wording in any commit message or branch comment**.
- One logical change per commit; every commit builds.
- Bootstrap (needs your OK because it lands a commit on `main`): initial commit on `main`,
  then `develop`, then M0 starts.

## 9. Security rules for this repo

- No secrets, tokens, or absolute private paths in any tracked file.
- No credentials in the remote URL; use a credential helper or SSH.
- Pre-commit eyeball: no `.env`, `*.pem`, `*.key`, `id_rsa`, build output — `.gitignore`
  covers `build/`, `out/`, `*.obj`, `*.exe`, `.cache/`, `compile_commands.json`.
- Server-side security is a feature, not an afterthought: path traversal, header
  smuggling (CRLF in any echoed value — nothing user-controlled is echoed raw), oversized
  heads/bodies, request smuggling, fd exhaustion caps.

## 10. Resolved decisions

1. Remote: `https://github.com/AgentDudu/http-server-cpp.git`, pushed via `gh` CLI
   credential helper — no tokens in remotes, configs, or files.
2. Default `--root ./www` with a bundled sample tree. Serving the current directory by
   accident is a footgun; a demo tree also showcases MIME/ETag/Range.
3. doctest v2.4.11 vendored in `external/doctest/` (single header + LICENSE.txt):
   offline configure, reviewable, pinned.
4. `main` is held. Bootstrap and all work happen on `develop`; `main` is created later
   by a release PR with explicit approval.
