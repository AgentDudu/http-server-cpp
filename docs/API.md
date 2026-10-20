# API reference

Four layers, strict one-way dependencies: `app → server → http → net`.
Only `net/` contains OS code. Everything below is header-exact as of this
commit; the headers carry the full contracts.

## `net` — platform seam

| Type | Notes |
| --- | --- |
| `net::Socket` | move-only handle wrapper. `tcp4()`, `bindAny(port)` (port 0 → ephemeral), `listen(backlog)`, `accept()`, `connect()`, `setNonBlocking()`, `setReuseAddress()`, `sendAll()`, `recvSome()` (kernel-accurate; `-1` + `ErrorKind::WouldBlock`), `localPort()`, `close()`. Factories return an invalid socket (`valid() == false`) instead of throwing. |
| `net::PollSet` / `net::pollSockets()` | thin WSAPoll/poll wrapper. `ErrorKind::Interrupted` (`EINTR`) is reported, never silently retried — the reactor treats it as a re-check. |
| `net::Context` | RAII WSAStartup / no-op on POSIX. One per process before sockets. |
| `net::lastError()` / `net::Error` | mapped error kinds shared by both backends. |

Contract that costs the most care: **an accepted socket inherits the
listener's blocking mode on Winsock and must not on Linux** —
`accept4(SOCK_CLOEXEC)` + explicit `O_NONBLOCK` copy keep the portable
behaviour identical. Linux sends use `MSG_NOSIGNAL`.

## `http` — protocol objects (no I/O)

```cpp
http::RequestParser parser;          // push parser, no sockets involved
parser.feed(bytes);                  // append raw bytes
switch (parser.next()) {             // loop until Request/Error
case Result::Request: {
    const http::Request& r = parser.request();   // owns its bytes (strings)
    bool ka = parser.keepAlive();                // valid right here
    parser.reset();                  // consumes this request, keeps pipeline remainder
} ...
}
```

- `Request`: `method`, `target`, `version`, `headers` (`http::Headers`,
  insertion-ordered, `find(name)` → index or `npos`), `body`. All owned —
  safe across `reset()` and further `feed()`s.
- `Limits` (per parser): `maxHeadBytes` 64 KiB, `maxBodyBytes` 8 MiB,
  `maxHeaderCount` 100.
- `Error` → connection mapping: `HeadTooLarge`/`TooManyHeaders` → `431`,
  `BodyTooLarge` → `413`, `VersionNotSupported` → `505`, else `400`; any
  error drops the connection after the best-effort reply.
- `idle()` — no request bytes since `reset()`: an idle timeout on such a
  stream closes quietly; a half-received request gets `408`.
- `http::Response` — status, headers (`setHeader` replace / `addHeader`
  append), body; `serialize()` / `serializeHead()`; `encodeChunk()` for
  chunked bodies (response-side only).
- `http::formatHttpDate` / `parseHttpDate` — IMF out; IMF + RFC 850 +
  asctime in (no libc `timegm`; portable civil-calendar math).

## `server` — threads and handlers

```cpp
server::StaticFiles files("./web");          // throws if root missing
server::Server server(8080, files);          // Handler = std::function<Reply(const Request&)>
server.threads = 0;                          // 0 = hardware_concurrency
server.run(signal);                          // blocks until stop requested
```

`Server` knobs: `idle_timeout` 5 s, `shutdown_grace` 5 s, `accept_batch` 64,
`max_queued_bytes` 1 MiB, `threads`. Constructor throws `std::runtime_error`
on bind/listen failure; `port()` reports the bound port (ephemeral-aware).

**Threading model (the rule that makes this race-free):** one acceptor
thread hands accepted sockets to K reactor threads over a mutex-guarded
inbox + wakeup socket. A `Connection` (socket + parser + output queue) is
owned by exactly one reactor forever; no connection state is ever shared or
locked. Handlers run inline on their reactor thread and must not block it.

`Reply{status, content_type, body, headers}` — HEAD drops the body but keeps
`Content-Length` honest (done in `Connection`; `304` omits both).
Handler exceptions are caught at the reactor seam → `500`.

`server::StaticFiles` — serve-a-root as a `Handler`: traversal refusal
(before any disk access), index.html, `405` + `Allow`, MIME, ETag/
Last-Modified/304, single-Range/206/416. Constructor canonicalises the root
(`std::filesystem::filesystem_error` if it doesn't exist).

`ShutdownSignal` — pluggable stop trigger (`stopRequested()`, optional
`preparePoll`/`afterPoll`/`pollCap`). Tests drive it directly for
deterministic graceful-stop coverage.

## `app` — signals

`app::ShutdownSignals` implements `ShutdownSignal` from SIGINT/SIGTERM
(self-pipe on POSIX — poll wakes instantly; console-control handler on
Windows — sets the flag, blocks until the loop ends, capped 100 ms idle
poll). `notifyStopped()` completes the Windows handler. Exit code 0.

## Error-handling conventions

- `net` factories: invalid handle + `lastError()`; no exceptions.
- `http` parser: `Result::Error` + `Error` enum; never throws on hostile
  input (suite-proven).
- `server`: constructor throws (config-level); runtime per-connection errors
  close the connection, never the reactor.
- Handler exceptions → `500`; `StaticFiles` I/O errors → `404`/`500`, never
  a leak of paths.
