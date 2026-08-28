# 0001 — content-addressed artifact store, published by symlink

## Status

Accepted. The image tier described here was removed in v2.0.0 --
see ADR-0006. The store model, symlink publishing and name handling
are unchanged.

## Context

This repository began as a hand-built static export: 5 whole-image rootfs
tarballs and 66 package tarballs pulled off a live host (192.168.15.95),
served by `python3 -m http.server`, with a hand-written `MANIFEST.json`
alongside them recording each artifact's version, size and sha256.

Two problems forced a real store.

**The package tier was unreachable.** A Cix host requests a package
artifact at `GET <base_url>/<name>-<version>.tar.gz` — flat, at the root
of the base URL (`pkg_artifact_build_request()`, `daemon/src/pkg.c:6976`
in the Cix repository). Images are requested one level down, at
`GET <base_url>/images/<name>-<hash>.tar.gz`. A directory tree with
`packages/` and `images/` beside each other can satisfy the second shape
or the first, never both: served as-is, packages appear at
`/packages/bash-5.2.37.tar.gz`, which is not a URL any host asks for. The
export shipped in exactly that state.

This was then confirmed live against a real install
(`docs/LAYOUT-CORRECTION.md`): a host pointed at the export requested
`GET /gcc-16.2.0-11.tar.gz`, got a 404, and silently rebuilt GCC from
source -- the multi-hour rebuild the cache exists to avoid. It surfaced
only because someone was watching an access log at the time. See ADR-0005
for what that near-miss changed.

**The manifest was a second source of truth.** It was written by hand at
export time. Nothing kept it in step with the files, and the moment
anything is published or removed it begins to describe a tree that no
longer exists. A manifest that disagrees with the bytes is worse than no
manifest, because it will be believed.

A third consideration: this store already contains near-duplicate
revisions of the same package (`bash-5.2.37` and `bash-5.2.37-2`,
`zlib-1.3.2` through `zlib-1.3.2-6`, and so on). Several are byte-identical
or nearly so, and every one of them costs a full copy.

## Decision

Store artifacts content-addressed, and publish names into that store as
**relative symlinks**:

```
<root>/blobs/<sha256>                    the bytes, mode 0444
<root>/packages/<name>-<version>.tar.gz  -> ../blobs/<sha256>
<root>/images/<name>-<hash>.tar.gz       -> ../blobs/<sha256>
<root>/tmp/                              upload staging, same filesystem
```

The URL namespace is then decoupled from the disk layout by the server:
`/<x>.tar.gz` resolves under `packages/`, `/images/<x>.tar.gz` under
`images/`. That repairs the package tier without moving a single file into
a shape that would break the image tier.

`MANIFEST.json` is **generated from the tree on request** rather than
stored. The hand-written file is kept in the repository as the export's
historical record — it is what the importer cross-checks each artifact's
bytes against — but it is not what gets served.

**Names are never split.** `<name>-<version>` cannot be split
unambiguously: `libc-dev-2.36` and `nss-pam-ldapd-0.9.13-2` both have
hyphens on both sides of the boundary, and a splitter that guesses wrong
would be a parsing bug sitting directly on the path-resolution hot path.
The whole basename is the key and is looked up literally. Validation is a
whitelist — `[A-Za-z0-9._-]`, a required `.tar.gz` suffix, no leading dot,
no `..` — so traversal is structurally impossible rather than filtered
against. Image-tier names must additionally end in `-<64 lowercase hex>`,
the only shape a host can compute.

### Why symlinks rather than hardlinks

Hardlinks would give a cheaper collector: `st_nlink` is the reference
count, so an unreferenced blob identifies itself. But a hardlink does not
encode the digest, so answering "what is this artifact's sha256" would
mean re-reading the file — 2.7 GB for the `cix-builder` image, on every
query.

A symlink's target *is* the digest. Resolving a name to its checksum and
size is `readlink()` plus `stat()`, with no index anywhere that can drift.
The cost is that garbage collection has to build the live set by reading
every published entry first, which is O(n) in the number of names. That
trade is the right way round: resolution happens on every request,
collection happens approximately never.

## Consequences

- Deduplication is free. Two names for the same bytes cost one blob, which
  is exactly what several of the existing package revisions are.
- Migration from the static export is `rename()` only. `tmp/`, `blobs/`
  and the tier directories share a filesystem, so the 4.6 GB is never
  copied. See ADR-0004 for the importer's cross-check against the shipped
  manifest.
- A blob is immutable by construction, so it is stored `0444`. Anything
  that wants to change an artifact must publish a different digest, which
  ADR-0003 then refuses under an existing name.
- Losing the store costs rebuild time and nothing else. It holds no
  information that is not derivable from git plus compute — which is the
  property DESIGN.md section 2 requires and ADR-0002 restates.
- A dangling symlink (a blob collected while a name still points at it) is
  reported as a `-1` size by the listing API rather than hidden. It should
  not be reachable, and if it is, it is a bug worth seeing.
