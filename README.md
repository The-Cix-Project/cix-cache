# cix-cache — a push/pull binary artifact registry for Cix

`cixcached` serves precompiled Cix package artifacts over the exact URLs an
unmodified Cix host already requests, so a new box can install packages
instead of rebuilding them from source.

It ships with `cix cache` (a REST client) and a web dashboard.

```
build/cixcached --root=cache --bind=0.0.0.0 --port=8080
```

## What it serves

| Purpose | Request |
|---|---|
| artifact | `GET <base>/<name>-<version>-<release>-<arch>.tar.gz` (or `.cixpkg`) |
| installer | `GET <base>/<name>-<version>-<release>-<arch>.iso` (+ `.iso.minisig`) |
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
Artifact paths are the only ones ending in a suffix the store
recognises — `.tar.gz`, `.cixpkg`, `.iso`, and a `.minisig` of any of
them — so the two namespaces cannot collide. The suffixes live in one
table rather than being spelled out at each site that needs to strip
one, and that table carries what the store knows about each: the tier
it belongs to, and whether it may be published unsigned.

`.cixpkg` is the format Cix is moving to (cix-build-system#141). Both
encodings of an artifact can exist at once during that migration, which
is why a listing row is an *artifact* carrying its formats rather than
a file. See `docs/adr/0012-cixpkg-and-signature-policy.md`.

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

Once stamped, a name also carries the machine it was built for —
`bash-5.2.37-1-x86_64` — spelled as `uname -m` spells it. A checksum
cannot tell an aarch64 binary from an x86_64 one, so the name is the
only thing that can. A bare name resolves while exactly one architecture
is published and **stops resolving** as soon as two are, rather than
picking. See `docs/adr/0008-architecture-in-artifact-names.md`.

The server wears this too. `cixcached` reports its build as
`cix-cache-v2.18.0-1-x86_64` — a name this store would accept, with a
release and an architecture — rather than as `git describe` output,
which had no room for either. See
`docs/adr/0013-the-daemon-wears-the-grammar-it-enforces.md`.

A push under a non-canonical name is **stored canonically**, and a fetch
under one is served as an **alias** of the canonical entry — one file,
one checksum, both spellings resolving to it. So recipes written before
the standard keep working while they are updated, rather than silently
falling back to a source build. Alias hits are logged and counted, since
a thing that works is otherwise invisible. See
`docs/adr/0007-canonical-artifact-names.md`.

## Installer ISOs

A bootable ISO is served too, with a detached minisign signature beside
it (`…-x86_64.iso` and `…-x86_64.iso.minisig`). That is not the image
tier coming back: an image can be composed by a running host, which is
why ADR-0006 removed it, and an ISO is what you boot to *create* a
host — there is nothing on the far side to compose anything.

Installers are counted apart from packages, the way `MANIFEST.json` has
always kept them in separate sections: an ISO is not resolved by
`name@version` and has no recipe behind it, so it is not one of the
"N packages".

An unsigned ISO is refused, because an ISO is fetched by a person and
booted with no recipe checksum vouching for it. Which suffixes are
refused unsigned is the `require_signature` setting, `.iso` by default;
a store can be told to demand one for packages too. It does not change
which tier an artifact is in — a signed package is still a package. **Verify before writing
the stick, on a machine you already trust** — the signature must be
checked by something other than the thing being checked, and an
installer validating itself proves nothing. See
`docs/adr/0010-installer-isos-are-served-and-signed.md`.

## Reading a listing

A row is one **artifact**, not one file. Where an artifact exists in
more than one encoding — a `.tar.gz` and a `.cixpkg` of the same
identity — it is one row listing both, because a format change is not a
new artifact. So `count` and `files` in `/api/v1/artifacts` are
different numbers, and a Format column appears only once something
actually holds two.

Listings come back **most recently published first**, so the front page
answers "what landed". Columns are sortable when you want a different
question answered — by name to scan for something, by size to find what
is taking up the room.

`MANIFEST.json` is ordered by name instead, because it is a document
meant to be compared against another copy of itself; ordered by time it
would diff as noise.

Versions compare by rule rather than by string, so `2.1.10` follows
`2.1.8` and a prerelease comes *before* its release. The rules, and
their limits, are in `docs/adr/0009-ordering.md`.

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
  packages/<name>-<ver>-<rel>-<arch>.tar.gz  -> ../blobs/<sha256>
  packages/<name>-<ver>-<rel>-<arch>.cixpkg  -> ../blobs/<sha256>
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
make          # cixcached, cix-cache, and the tests
make clean
```

Tests are standalone binaries, several of which drive a real server on
a dedicated port:

```
make test     # stops at the first failure
```

They can be run individually too — `build/test_store` and the rest —
but `make test` is the one that fails loudly, and it takes its list
from the Makefile rather than from a copy of it here.

### Fuzzing

The HTTP parser and the name grammar read attacker-controlled bytes
before anything has authenticated the request, so both are fuzzed:

```
make fuzz                   # build the harnesses (needs clang)
make fuzz-check             # a short run over the committed corpus
```

This does not change what ships. TCC has no sanitizers, so the
harnesses in `fuzz/` are built by clang with ASan and UBSan — a
developer tool that `make all` does not depend on and `make install`
never touches. Every clang flag in the repository lives in
`fuzz/Makefile`, in one file, so the boundary is visible rather than
assumed. The daemon is a TCC build and only a TCC build; see
`docs/adr/0014-fuzzing-under-clang.md`.

`fuzz_name` asserts properties, not just the absence of crashes — that
canonicalisation cannot emit a name the store rejects, and that it
settles. A canonicaliser that renames a published artifact into
something no request can resolve would never crash.

Anything a harness finds gets a case in the ordinary C test suite as
well as a corpus seed: `fuzz/corpus/` needs clang to catch a
regression, and `make test` is what a deployment gates on.

## Documentation

The dashboard carries its own reference at `/#help` — endpoints, status
codes, CLI commands and how the store works.

- `docs/DESIGN.md` — the original design brief and its reasoning
- `docs/DEPLOYMENT.md` — running it, and pointing hosts at it. A public
  instance behind Caddy is one re-runnable script: `deploy/deploy.sh`,
  and `tools/seed.sh` fills it from an instance you already run
- `docs/adr/` — one decision per file, with an index in
  `docs/adr/README.md`. Their figures are dated observations rather than
  current facts: an ADR records the evidence a decision was made on, so
  the numbers are left alone and date-stamped rather than refreshed.
