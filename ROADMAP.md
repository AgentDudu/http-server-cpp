# Roadmap after v0.1.0

Recorded 2026-10-20, right after the v0.1.0 release. Ordered by recommendation;
"gap" items are deviations already noted in PLAN.md and README. Each line is
its own feature branch (gitflow as usual, `main` merges need approval).

## Close the recorded gaps

1. **Bounded file streaming** (recommended first)
   - `StaticFiles` reads whole files into memory; noted in code and README.
   - Cap memory per response, improve time-to-first-byte on large files.
   - Must handle file shrink/grow between stat and send (the 206 race guard
     already degrades to 416; streaming needs the same honesty).
   - Tests: large-file memory ceiling, mid-send truncate, slow reader + range.

2. **Windows benchmark numbers**
   - PLAN.md records this deviation; Linux numbers are in README.
   - Needs a portable load client: tiny C++ bench tool or a PowerShell
     `HttpClient` loop. Half a day.

3. **Access logging + real flag parsing**
   - Today: no logging, three positional args (`port root threads`).
   - `--access-log`, `--root`, `--threads`, `--help`; small hand-rolled parser,
     no dependencies.

## Durability before calling it production

4. **Connection limits**
   - Max concurrent connections per server (accept then immediately close past
     the cap); fd-limit check at startup; 10k-connection load test.
   - Slow clients are already bounded per connection; nothing bounds total
     sockets yet.

5. **Response-shape fuzz**
   - Request corpus exists (`tests/test_smuggling.cpp`); add a HEAD/GET/304/206
     matrix differ that asserts `Content-Length`/body consistency across every
     reply path.

6. **`epoll` backend behind the `net` seam**
   - `poll()` is O(n) per wakeup; the seam already isolates the OS call.
   - Matters past roughly 5k connections per reactor. Keep poll for macOS and
     Windows (WSAPoll).

## Feature scope decisions (discuss before branching)

7. **Chunked request bodies**
   - Parser rejects `Transfer-Encoding` today (safe default). Required before
     any POST endpoint.

8. **POST / routing surface**
   - The `Handler` seam exists; router + middleware shape is design work, not
     code work.

9. **TLS**
   - Kills the dependency-free rule: schannel on Windows, OpenSSL/BoringSSL on
     Linux. Biggest single decision in the project; write an RFC section in
     PLAN.md before any branch exists.

## Repo polish (15 minutes each)

10. CI status badge (now meaningful on `main`) and release badge in README.
11. `CHANGELOG.md` generated from `git log v0.1.0` onward.
12. `SECURITY.md`: smuggling corpus, traversal tests, sanitizer gates are a
    real posture worth stating.
13. Branch protection on `main` (require CI before merge) for future
    collaborators.

## Suggested order

1 -> 4 -> 10 -> 2, then the 7/8/9 discussions. Streaming and connection caps
are the two things separating "demos well" from "production".
