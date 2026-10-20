# Architecture

Placeholder. Each subsystem documents itself here as it lands; PLAN.md sections 3-4
describe the intended design until then.

- `net/` — Winsock RAII, non-blocking sockets, WSAPoll reactor (landed)
  - `WsaContext` — process-wide Winsock 2.2 session, one per process.
  - `Socket` — move-only handle; non-blocking toggle, reuse-addr, bind/listen/accept,
    connect; `sendAll`/`recvSome` helpers. Errors via `WSAGetLastError()` immediately
    after a failed call.
  - `PollSet` + `pollSockets()` — thin `WSAPoll` wrapper; no `FD_SETSIZE` ceiling.
- `http/` — request parser, response writer, headers, URL, MIME
  - landed: `Status` + reason table; `Headers` (ordered, case-insensitive, trimmed);
    `Response` with strict head serialization — CTL/CR/LF/NUL or bad names fail the
    call instead of reaching the wire; Content-Length implied unless explicit or
    chunked; 204/304/1xx bodies refused; lowercase-hex chunk encoder.
  - pending: request parser, URL decode/normalise, MIME.
  - `http/` builds warning-clean and ASan/UBSan-clean on Linux (`tools/ck-asan-docker.sh`);
    `net/` and the server binary are `if(WIN32)` targets.
- `server/` — listener, static file handler, config (pending)

Accepted sockets inherit the listener's non-blocking mode on Windows; callers set the
mode they want after `accept()`.
