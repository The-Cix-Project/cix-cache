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

**Every artifact miss logs the exact request that missed**, method and
path verbatim, plus the reason (`not found` versus `invalid artifact
name` — the second means the client asked for something that could never
exist, which is a different problem):

```
cixcached: MISS GET /gcc-16.2.0-11.tar.gz (not found)
```

**Hits and misses are counted separately** and exposed as
`artifact_hits` / `artifact_misses` on `GET /api/v1/status`, in
`cixcachectl status`, and on the dashboard.

The dashboard colours the counter as a warning when there are misses and
**zero** hits, because that specific combination is the signature of a
registry nobody can reach. Some misses are normal — they mean a package
genuinely is not cached yet. Only-misses means the layout or the base URL
is wrong.

## Consequences

- `test_serve` asserts both counters move, so the signal cannot be
  refactored away silently.
- Miss logging is one `fprintf` per miss on a path that is already doing
  a `readlink`, so the cost is irrelevant next to serving an artifact.
- This is the observability half of the layout fix. The other half is
  structural: the server maps URLs onto the store itself (ADR-0001), so
  the layout that caused this cannot recur by rearranging directories.
- A `MISS` line is not an error and should not page anyone. It is the
  record of a question asked and answered "no" — useful precisely because
  the answer is invisible everywhere else.
