# http-server-cpp

A dependency-free HTTP/1.1 file server in C++20 — Windows and Linux from one
codebase. Acceptor thread + K single-threaded reactor threads, one push
request parser, one static-file handler. No frameworks, no libuv, no Boost.

## What works today

- HTTP/1.1 + 1.0: keep-alive, pipelining, HEAD, `408` on stalled requests,
  silent close on idle keep-alive streams (RFC 9112).
- Request parser hardened against request smuggling: `CL+TE`, duplicate /
  signed / garbage `Content-Length`, bare-LF endings, obsolete header
  folding, NULs, non-token names/methods all rejected — and the stream is
  dropped after any framing error (never answered and reused).
- Static files: traversal-proof resolver (strict percent-decode, canonical
  containment, Windows case-folded), `405` + `Allow`, MIME table.
- Caching & ranges: strong `ETag` + `Last-Modified`, `If-None-Match` /
  `If-Modified-Since` → `304` (before the file is read), single `Range` →
  `206` / `416` with `If-Range` gating.
- Slow-client safety: per-connection deadlines, per-connection output cap
  (`max_queued_bytes`) with throttling — one stalled reader never blocks a
  reactor or another connection.
- Graceful `Ctrl-C` / SIGINT / SIGTERM: queued responses drain, then exit 0.

## Quick start

```sh
# Linux
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
./build/http_server 8080 ./web          # serve ./web on :8080

# Windows (MinGW-w64 on PATH; MSVC preset: -preset msvc-x64-release)
cmake --preset mingw-x64-release && cmake --build --preset mingw-x64-release
build\mingw-x64-release\Release\http_server.exe 8080 .\web
```

`http_server [port] [root] [threads]` — threads `0` (default) means
`hardware_concurrency`.

## Benchmarks

Indicative only — server and benchmark clients share one WSL2 container
(Ubuntu 24.04, gcc 13 `-O2`, 56 visible cores); numbers scale with the host.
Reproduce with `tools/bench-inside.sh` inside any Linux container with
`wrk` + `apache2-utils`.

| Scenario                            | 1 reactor              | 4 reactors             |
| ----------------------------------- | ---------------------- | ---------------------- |
| `wrk -t4 -c100`, 1 KiB keep-alive   | 9,770 req/s · p99 19.8 ms | 47,275 req/s · p99 4.3 ms |
| `wrk -t4 -c100`, 64 KiB keep-alive  | 5,683 req/s · 356 MB/s | 25,919 req/s · 1.59 GB/s |
| `ab -c50`, 1 KiB fresh connections  | 8,027 req/s            | 8,443 req/s            |

Reactor count scales near-linearly for keep-alive traffic; fresh-connection
throughput is acceptor/handshake-bound by design (single acceptor). Soak
signal (CI `linux-soak` job): 109,110 mixed-churn request iterations in 60 s,
0 errors, RSS drift 2.3 MB.

## Limits & behaviour

| Knob                 | Default              | Effect                                        |
| -------------------- | -------------------- | --------------------------------------------- |
| request head         | 64 KiB               | larger → `431` then drop                      |
| declared body        | 8 MiB                | larger → `413` then drop                      |
| header count         | 100                  | more → `431` then drop                        |
| `idle_timeout`       | 5 s                  | stalled request → `408`; idle stream → close  |
| `max_queued_bytes`   | 1 MiB                | output backlog before throttling the reader   |
| `shutdown_grace`     | 5 s                  | drain budget on stop                          |

Not served: `Transfer-Encoding: chunked` requests, multi-range, POST bodies.
Static handler reads whole files into memory (bounded streaming is planned).

## Layout & docs

```
src/net/     platform seam: sockets, poll, errors (raw_win / raw_posix)
src/http/    parser, headers, response, status, HTTP-date
src/server/  acceptor + reactors, connection, static handler
src/app/     shutdown signals (self-pipe on POSIX, console ctrl on Windows)
tests/       doctest suite incl. framing corpus, threading, env-gated soak
```

[ARCHITECTURE.md](docs/ARCHITECTURE.md) · [BUILDING.md](docs/BUILDING.md) ·
[API.md](docs/API.md)

## Verification

Every push runs: Windows MinGW build + full suite; Linux ASan+UBSan and TSan
(container); 60 s RSS soak. Locally: `tools/ck.ps1` (Windows) and
`tools/ck-asan-docker.sh` (Linux). See [BUILDING.md](docs/BUILDING.md).
