# cix-cache — a push/pull binary artifact registry for Cix

`cixcached` serves precompiled Cix package artifacts over the exact URLs an
unmodified Cix host already requests, so a new box can install packages
instead of rebuilding them from source.

It ships with `cixcachectl` (a REST client) and a web dashboard.

```
build/cixcached --root=cache --bind=0.0.0.0 --port=8080
```

## What it serves

| Purpose | Request |
|---|---|
| artifact | `GET <base>/<name>-<version>.tar.gz` |
| publish | `PUT`, with `X-Cix-Sha256` |
| existence probe | `HEAD` |
| auth | `Authorization: Bearer <token>` |

Packages are the only tier. An image is a recipe composed of packages —
its version is the hash of its own package manifest — so a whole-rootfs
tarball would duplicate bytes this store already holds. Hosts compose
images from these. See `docs/adr/0006-packages-are-the-only-tier.md`.

Everything else is for people: `/` is the dashboard, `/api/v1/*` is
observability, and `/MANIFEST.json` is generated from the tree on request.
The store holds several revisions of most packages, so a count of
packages and a count of published artifacts are different facts and both
are reported.
Artifact paths are the only ones ending `.tar.gz`, so the two namespaces
cannot collide.

## Artifact names

Canonical identity is **`<name>-<version>-<release>`**, and an omitted
release means `1`:

```
gawk-5.3.0-7        name=gawk    version=5.3.0     release=7
mtools-4.0.49-1     name=mtools  version=4.0.49    release=1
cix-v2.2.0-rc6-1    name=cix     version=v2.2.0-rc6  release=1
```

`version` is upstream's, verbatim. `release` is Cix's own packaging
revision of that version, and moves when the recipe changes and upstream
does not.

A push under a non-canonical name is **stored canonically**, and a fetch
under one is served as an **alias** of the canonical entry — one file,
one checksum, both spellings resolving to it. So recipes written before
the standard keep working while they are updated, rather than silently
falling back to a source build. Alias hits are logged and counted, since
a thing that works is otherwise invisible. See
`docs/adr/0007-canonical-artifact-names.md`.

## The invariant

The registry is **never a trust boundary**, and it is a **cache, never a
catalogue**.

1. the recipe comes from git — versioned, reviewable text;
2. the artifact comes from here — opaque bytes;
3. the recipe's own checksum validates the artifact.

So no Cix host code path may ever consult the listing API. A host fetches
one exact name it already learned from a recipe, and verifies the bytes
itself. Losing this store entirely must cost rebuild time and nothing else.

**Recipes must never be served from here.** That is the one change that
silently destroys the model, and it has already been attempted once — the
image recipes were originally generated into this directory, next to the
binaries they validate. See `docs/adr/0002-registry-is-a-cache-not-a-catalogue.md`.

## Store layout

```
cache/
  blobs/<sha256>                    the bytes, mode 0444
  packages/<name>-<version>.tar.gz  -> ../blobs/<sha256>
  tmp/                              upload staging
```

The symlink target *is* the digest, so a name's checksum and size are known
without an index that could drift and without re-reading the file.
Duplicate content costs one blob. The server maps URLs onto this layout,
which is what lets artifacts be served flat at the root of `base_url`
while being stored in a directory of their own.

## Building

TCC only, dynamically linked against system glibc:

```
make          # cixcached, cixcachectl, and the tests
make clean
```

Tests are standalone binaries run by hand, against a real server on a
dedicated port:

```
for t in store http manifest import serve push gc; do build/test_$t; done
```

## Documentation

The dashboard carries its own reference at `/#help` — endpoints, status
codes, CLI commands and how the store works.

- `docs/DESIGN.md` — the original design brief and its reasoning
- `docs/DEPLOYMENT.md` — running it, and pointing hosts at it
- `docs/adr/` — why the store, the reactor and the push protocol are shaped
  the way they are
