# Correction: package artifacts live at the ROOT of base_url

Found live on 2026-08-26, testing a real install against this cache.

## What was wrong

The exported tree put package artifacts under `packages/`:

    cache/
      packages/gcc-16.2.0-11.tar.gz     <-- WRONG
      images/gcc-tcc-bootstrap-<hash>.tar.gz

A real Cix host pointed at that base_url requested:

    GET /gcc-16.2.0-11.tar.gz  ->  404

and, finding nothing, silently fell through to building GCC from source
-- which is exactly the multi-hour rebuild the cache exists to avoid.

## The correct layout

ADR-0122 computes a package URL as `<base_url>/<name>-<version>.tar.gz`
with **no prefix at all**. Only whole-image artifacts get a
subdirectory, `<base_url>/images/<name>-<image_version>.tar.gz`
(ADR-0123), specifically so the two namespaces cannot collide while
sharing one base_url and token.

So the served tree must be:

    <base_url>/
      <name>-<version>.tar.gz          <-- packages, at the root
      images/<name>-<hash>.tar.gz      <-- images, one level down

The current directory keeps `packages/` as storage and symlinks each
entry into the root, which satisfies both. A purpose-built registry
should just serve the right paths directly.

## Why this matters more than a path typo

**A missing artifact is not an error.** The daemon treats a 404 as
"no artifact available" and builds from source instead -- correct
behaviour (an artifact server must never be able to fail a build), but
it means a wrong layout looks like a working cache that simply never
gets used. There is no failure to notice.

Two consequences for the registry design:

1. Serve the ADR-0122/0123 paths **exactly**. They are computed
   independently by every client from recipe text alone; a server that
   invents its own layout is silently unreachable.
2. Give operators a way to tell "not cached" from "cached but
   unreachable" -- a hit/miss counter, or a log line naming the URL that
   404'd. The only reason this was caught at all was watching the
   server's access log during a test install and seeing the 404.
