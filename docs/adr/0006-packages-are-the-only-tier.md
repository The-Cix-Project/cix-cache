# 0006 — packages are the only tier

## Status

Accepted. Supersedes the image-tier half of ADR-0001, ADR-0002 and
ADR-0003.

## Context

The registry served two tiers, mirroring the two the Cix daemon can
fetch: individual packages at the root of `base_url`, and whole-image
rootfs tarballs under `images/` (its ADR-0122 and ADR-0123).

An image is not an independent thing. It is a **recipe composed of
packages** — a list of `name:mode:version` entries — and its version is
literally the sha256 of that sorted `name@version,…` manifest. Nothing
about an image exists that is not either in the recipe (in git) or in
the packages it names.

So a whole-image tarball is a derived artifact, and holding one meant
holding bytes twice. Measured against the store at the time of this
decision: the `dev` image was 330 MB, and **all 18 packages composing it
were already present in the package tier**. Across all five images that
was 3.85 GB of the 4.5 GB store — 85% of it — to avoid re-expanding
package sets this registry already had.

It also cost more than space. The image tier carried its own name
grammar (`<name>-<64 hex>.tar.gz`), its own manifest-hash arithmetic
that had to match the daemon byte for byte, its own section in
`MANIFEST.json`, its own CLI flag, and its own column handling in the
dashboard — a second shape running through every layer for something
that composes from the first.

The failure modes differ too, and not in the image tier's favour. A
missing package is harmless: the daemon silently builds from source. A
missing image is **not** a fallback — the daemon leaves the image
untouched and records `failed`. Serving a tier where absence is an error
is a strictly worse contract for a cache.

## Decision

**Serve packages, and only packages.** Hosts compose images from them.

Removed entirely rather than left dormant: `enum store_tier`, the
`images/` store directory, the `/images/…` route, the `images` section
of `MANIFEST.json`, `cixcachectl --images`, and the dashboard's tier
column and chips. A path with a directory component now names nothing
this registry has, so `/images/anything.tar.gz` misses like any other
unknown name — which is the correct answer, not a special case.

The `packages` wrapper in `MANIFEST.json` stays even though it is now
the only section, so anything already reading `.packages` keeps working.

`test_contract` is deleted with the tier. It re-derived an image version
from recipe text and checked it against one captured from a live host,
which proved the image URL was byte-exact — a good test for a contract
that no longer exists. The package URL shape it shared with `test_serve`
is still covered there.

### What this requires elsewhere

Five recipes in `itdlabs/cix` still declare `image_artifact_sha256`:

```
recipes/image/{jumpbox,cix-builder,cix-hosttools,dev}/2.0.0/build.sh
recipes/image/gcc-tcc-bootstrap/1.0.0/build.sh
```

While that line is present the daemon arms ADR-0123's fast path, and a
fetch that now 404s makes an `apply-recipe` **fail** rather than fall
back. Removing the line from those five is the other half of this
change and must land for them to build from source instead.

## Consequences

- The store dropped from 4.5 GB to 919 MB, holding 88 artifacts.
- One name grammar, one tier, one column. The manifest-hash arithmetic
  the server had to reproduce is gone with the tier that needed it.
- Rebuilding an image now costs assembling it from cached packages
  rather than one download. That is the trade this ADR accepts: the
  packages are cached, so nothing is compiled from source that was not
  before — only re-expanded.
- The five image tarballs were deleted from this store. They remain in
  the pre-migration backup at `~/cix-cache.backup`, including
  `gcc-tcc-bootstrap`, which holds a gcc self-hosted from TCC with no
  external compiler in its lineage and represents days of compute. If
  that one is ever wanted again it should be preserved deliberately
  somewhere, not left depending on a backup directory.
- This narrows what the registry is, which is the direction ADR-0002
  already pointed: it caches build outputs, and an image is not a build
  output but a composition of them.
