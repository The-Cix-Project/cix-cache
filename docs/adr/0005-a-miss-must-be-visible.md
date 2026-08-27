# 0005 — a miss must be visible

## Status

Accepted

## Context

An artifact server must never be able to fail a build. The Cix daemon
treats any status >= 400 identically as "no artifact available" and falls
through to fetching sources and compiling. That is correct, and ADR-0002
depends on it: a 404 is an ordinary, cheap answer meaning "build it from
source".

It also has a consequence that was found the hard way rather than
reasoned about in advance (`docs/LAYOUT-CORRECTION.md`, 2026-08-26). The
hand-built export served package artifacts under `packages/`, while every
host computes them at the root of `base_url`. So a real install requested
`GET /gcc-16.2.0-11.tar.gz`, received a 404, and quietly rebuilt GCC from
source over several hours.

Nothing failed. Nothing logged an error. The registry was up, healthy, and
serving 4.6 GB of artifacts that no host could reach. From the outside it
was indistinguishable from a working cache that simply had not been asked
for anything yet, and it stayed that way until somebody happened to be
watching an access log during a test install.

The generalisation is what matters: **because a miss is designed to be
harmless, a misconfigured registry is silent.** Every failure mode of this
server — wrong layout, wrong base URL, an artifact never pushed, a name
that does not match what the recipe computes — presents identically as
"quiet, and doing nothing useful".

## Decision

Distinguish "not cached" from "cached but unreachable" from the outside.

**Every request is logged with the exact path, its status, and whether
an artifact lookup hit or missed**:

```
GET /bc-1.08.1-2.tar.gz  -> 200 HIT 137115 bytes
GET /gcc-16.2.0-11.tar.gz -> 404 MISS 21 bytes
```

The status code carries the distinction this ADR originally spelled out
in words: 404 is "not cached", 400 is "that name could never exist" —
a different problem, and one the client caused.

This started as a dedicated `MISS` line separate from any access log.
Once the server grew a general activity log there were two lines per
missed request saying the same thing, so the miss became an annotation
on the access line instead. One line per request, still naming the
exact URL, which is all this ADR ever required.

**Hits and misses are counted separately** and exposed as
`artifact_hits` / `artifact_misses` on `GET /api/v1/status`, in
`cixcachectl status`, and on the dashboard.

**The log is reachable without shell access.** It is kept in a bounded
in-memory ring and served from `GET /api/v1/log?after=<seq>`, which the
dashboard displays and `cixcachectl log -f` follows. Everything in the
ring is also written to stderr, so the journal remains the durable
record and the ring is only a convenience -- an operator watching a
first install should not have to `ssh` somewhere to see whether the
registry is being reached.

The dashboard colours the counter as a warning when there are misses and
**zero** hits, because that specific combination is the signature of a
registry nobody can reach. Some misses are normal — they mean a package
genuinely is not cached yet. Only-misses means the layout or the base URL
is wrong.

## Consequences

- `test_serve` asserts both counters move and that the log records
  activity, so the signal cannot be refactored away silently.
- Logging is one formatted line per response on a path that is already
  doing a `readlink` and a `sendfile`, so the cost is irrelevant next to
  serving an artifact.
- The ring is bounded and overwritten oldest-first. A registry that ran
  out of memory keeping a record of serving artifacts would be an absurd
  way to fail, so the log can lose history but never grow without limit.
- This is the observability half of the layout fix. The other half is
  structural: the server maps URLs onto the store itself (ADR-0001), so
  the layout that caused this cannot recur by rearranging directories.
- A `MISS` line is not an error and should not page anyone. It is the
  record of a question asked and answered "no" — useful precisely because
  the answer is invisible everywhere else.
