# Architecture

Placeholder. Each subsystem documents itself here as it lands; PLAN.md sections 3-4
describe the intended design until then.

- `net/` — Winsock RAII, non-blocking sockets, WSAPoll reactor (landed)
  - `WsaContext` — process-wide Winsock 2.2 session, one per process.
  - `Socket` — move-only handle; non-blocking toggle, reuse-addr, bind/listen/accept,
    connect; `sendAll`/`recvSome` helpers. Errors via `WSAGetLastError()` immediately
    after a failed call.
  - `PollSet` + `pollSockets()` — thin `WSAPoll` wrapper; no `FD_SETSIZE` ceiling.
- `http/` — request parser, response writer, headers, URL, MIME (pending)
- `server/` — listener, static file handler, config (pending)

Accepted sockets inherit the listener's non-blocking mode on Windows; callers set the
mode they want after `accept()`.
