# cix-cache — a push/pull binary artifact registry for Cix

`cixcached` serves precompiled Cix artifacts over the exact URLs an
unmodified Cix host already requests, so a new box can install packages and
whole images instead of rebuilding them from source.

It ships with `cixcachectl` (a REST client) and a web dashboard.

```
build/cixcached --root=cache --bind=0.0.0.0 --port=8080
```

## What it serves

| Purpose | Request |
|---|---|
| package artifact | `GET <base>/<name>-<version>.tar.gz` |
| image artifact | `GET <base>/images/<name>-<hash>.tar.gz` |
| publish | `PUT` on either, with `X-Cix-Sha256` |
| existence probe | `HEAD` on either |
| auth | `Authorization: Bearer <token>` |

`<hash>` is not a label. It is the sha256 of the image's sorted
`name@version,…` manifest string, computed on the host from recipe text
alone — so the URL is derivable before the artifact exists. `test_contract`
re-derives it independently and checks it against a version captured from a
live host.

Everything else is for people: `/` is the dashboard, `/api/v1/*` is
observability, and `/MANIFEST.json` is generated from the tree on request.
Artifact paths are the only ones ending `.tar.gz`, so the two namespaces
cannot collide.

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
  images/<name>-<hash>.tar.gz       -> ../blobs/<sha256>
  tmp/                              upload staging
```

The symlink target *is* the digest, so a name's checksum and size are known
without an index that could drift and without re-reading the file.
Duplicate content costs one blob. The server maps URLs onto this layout,
which is what lets packages be served flat at the root while images sit
under `images/`.

## Building

TCC only, dynamically linked against system glibc:

```
make          # cixcached, cixcachectl, and the tests
make clean
```

Tests are standalone binaries run by hand, against a real server on a
dedicated port:

```
for t in store http manifest import serve push gc contract; do build/test_$t; done
```

## Documentation

- `docs/DESIGN.md` — the original design brief and its reasoning
- `docs/DEPLOYMENT.md` — running it, and pointing hosts at it
- `docs/adr/` — why the store, the reactor and the push protocol are shaped
  the way they are
