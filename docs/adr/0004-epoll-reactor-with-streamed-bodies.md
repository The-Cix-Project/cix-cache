# 0004 — one epoll reactor, with streamed bodies

## Status

Accepted

## Context

The obvious move was to reuse the Cix daemon's HTTP layer wholesale. Read
directly, it turns out to encode decisions made for the opposite workload:

- `HTTP_MAX_REQUEST_SIZE` caps an entire request at 1 MiB.
- `http_write_response()` takes the whole body as a pointer in memory.
- `static_serve()` `fstat`s a file, `malloc`s its full size, reads it in
  and writes it out.
- Its own header says so explicitly, as a scope boundary rather than an
  oversight: no chunked transfer-encoding, no keep-alive.

That is entirely reasonable for a control plane whose largest response is
a JSON list. It is unusable here, where the *smallest* artifact is 48 KB
and the largest is a 2.7 GB image rootfs. `malloc(2.7 GB)` per request is
not a tuning problem.

The daemon itself already declined to stream from its event loop: for
image export it exposes client-driven ranged chunking
(`GET /v1/images/{name}/export/download?offset=N&length=M`, 8 MiB cap)
rather than a streaming response, on the stated grounds that a single
event loop cannot afford to write a multi-GB body inline.

So the question was whether to keep the reactor shape at all.

## Decision

Keep the single-threaded epoll reactor — same `enum conn_kind` tagged
`struct conn` on `ev.data.ptr`, same 1 s `epoll_wait` tick, same deferred
teardown through a pending-free list — and make the **body** streamable
rather than the architecture different.

**Parsing completes on headers, not on the body.** `http_conn_try_parse()`
returns as soon as the blank line is seen. The caller then decides: a
`PUT` body is written straight to `tmp/` as it arrives and never touches
the parse buffer. Only the header section is size-capped
(`HTTP_MAX_HEADERS`), so a client that never sends a blank line cannot
grow the buffer without bound, while a 2.7 GB upload costs 64 KB of
transient read buffer.

**Responses stream via non-blocking `sendfile()`** through an explicit
state machine:

```
CONN_SEND_HEADER  partial header writes, then ->
CONN_SEND_BODY    sendfile(fd, blob_fd, &off, SENDFILE_CHUNK) until done
```

`SENDFILE_CHUNK` is 4 MiB. On a non-blocking socket `sendfile` returns as
soon as the socket buffer fills, so this does not bound a slow client — it
bounds a *fast* one, which could otherwise sit in a single syscall pushing
hundreds of megabytes into a large window while every other connection
waits.

Small bodies (API JSON, dashboard assets) take the same state machine with
a buffer instead of a blob fd. Dashboard assets are still read whole, the
one place buffering is genuinely simpler than streaming.

**Long-running children are pidfds in the same loop.** Hashing an upload
and running an import both fork; both register `pidfd_open()` in epoll
rather than blocking. This is the pattern the Cix daemon already uses for
its own package and image fetch children. When the pidfd fires the child
has exited, so its ≤65 byte pipe output is already buffered and one small
read cannot block.

**Transfers time out here, because they cannot time out anywhere else.**
The daemon sets no timeout on an artifact fetch: no `--max-time`, no
`--connect-timeout`, and no retry on either tier. A response that stalls
forever therefore does not fail on the host — it wedges that host's fetch
job indefinitely. Every connection carries a `last_progress_ms` updated on
each byte moved, and the existing 1 s tick closes anything past
`IDLE_TIMEOUT_MS` (60 s while reading) or `XFER_TIMEOUT_MS` (300 s while
transferring). This is not defensive polish; it is the only place the
bound can exist.

**Never redirect off-host.** `curl -L` follows redirects but does not
re-send the `Authorization` header to a different host unless
`--location-trusted`, which the daemon does not pass. A cross-host
redirect would silently drop the bearer token.

## Consequences

- Memory is bounded and roughly constant regardless of artifact size. A
  2.7 GB transfer costs one fd, one `struct conn` and a 4 MiB kernel-side
  copy window.
- The partial-`sendfile` resumption is the highest-risk code here, so
  `test_serve` pushes and re-fetches a 64 MiB artifact through real curl
  and compares sha256 — a size well past any socket buffer, so the
  transfer must survive dozens of `EAGAIN`/`EPOLLOUT` round trips.
- `struct epoll_event` must not come from the system header. TCC ignores
  `__attribute__((packed))` entirely, which would compile the 12-byte
  kernel ABI struct as 16 bytes with `data` at the wrong offset and
  silently corrupt every dispatch pointer. `include/linux_compat.h`
  carries the `#pragma pack` replacement from the Cix repository, where
  this was found the hard way (its ADR-0008).
- Still no keep-alive and no chunked encoding: every response closes the
  connection. The only client that matters is `curl -fsSL`, which needs
  neither.
- One slow client cannot stall others, but one *very* fast client shares
  the loop with everyone else. If this ever serves enough concurrent
  multi-GB pulls for that to matter, the answer is more processes behind a
  balancer, not threads in this one.
