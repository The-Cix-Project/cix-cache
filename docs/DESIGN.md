# cixcache — a push/pull binary artifact registry for Cix

**Status:** built. `cixcached`, `cixcachectl` and the dashboard implement
this design; the hand-made static export it describes has been migrated
into the content-addressed store. The decisions taken while implementing
it -- and the two places reality differed from this brief -- are recorded
in `docs/adr/`. This document is kept as written, as the reasoning that
produced the system.

**Read this first if you are picking the project up cold.** It is written
to be self-contained: it states the constraints that are not negotiable,
the reasoning behind them, and the mistakes already made so they are not
repeated.

---

## 1. What exists today

Cix already has the *consuming* half of an artifact tier, shipped and
working (ADR-0122 for packages, ADR-0123 for whole images):

* A host is pointed at one plain-HTTP base URL
  (`cixctl pkg artifact-config set --base-url=…`, optional bearer token).
* Before building a package from source, it tries
  `GET <base_url>/<name>-<version>.tar.gz`.
* For a fully-pinned image recipe it tries
  `GET <base_url>/images/<name>-<image_version>.tar.gz`
  and, on success, extracts that tarball as the image's **entire** rootfs.
* In both cases the fetched bytes are verified against a checksum the
  **recipe** declares (`pkg_artifact_sha256=` / `image_artifact_sha256=`),
  and are discarded on mismatch.

What has never existed is anything that *produces* or *serves* those
files. Today this directory is a hand-made static tree behind
`python3 -m http.server`. That works, and it is not a service.

## 2. The invariant that must not break

Cix deliberately splits recipes and binaries across **two independent
URLs**, and this is a security property, not a filing convention:

1. **the recipe comes from git** — versioned, reviewable, diffable text;
2. **the artifact comes from this registry** — opaque bytes;
3. **the recipe's checksum validates the artifact.**

Therefore **the registry is never a trust boundary.** It does not decide
what is legitimate; the recipe in git does. A registry that also served
the recipes would be vouching for its own payload, which is worth
nothing.

This is easy to violate by accident. It already happened once during the
export work: the image recipes were first generated *into this
directory*, next to the binaries they validate. That would have shipped
the checksum and the payload from the same server. They now live in the
repository (`recipes/image/*`), where they belong. Expect this mistake to
recur under time pressure; design against it.

A second, weaker invariant follows from the first: **the registry is a
cache, never a catalogue.** Git is the record of what exists. Losing the
registry entirely must cost only rebuild time — never correctness, and
never knowledge of what a package is. If anything ever has to consult
the registry to answer "does this package exist", the split has been
broken.

## 3. Hard compatibility constraints

The existing daemon is already a client. Do not require a daemon change
to consume this — it must serve, verbatim:

| Purpose | Path |
|---|---|
| package artifact | `GET <base>/<name>-<version>.tar.gz` |
| image artifact | `GET <base>/images/<name>-<image_version>.tar.gz` |
| auth (optional) | `Authorization: Bearer <token>` |

`<image_version>` is not a label — it is the hash of that image's sorted
`name@version` manifest, computed identically on both sides, so the URL
is derivable from recipe text alone. Anything that changes how it is
computed silently breaks every fetch.

Content is a gzipped tarball whose members sit at top level (`./usr/…`),
because the image path extracts it directly as a rootfs.

## 4. What to add: push

The natural producer is a host that has just built something. Suggested
shape, deliberately small:

* `PUT <base>/<name>-<version>.tar.gz` — store a package artifact.
* `PUT <base>/images/<name>-<hash>.tar.gz` — store an image artifact.
* `HEAD` on either — cheap "do you already have this?", so a builder can
  skip uploading hundreds of MB it does not need to.
* Reject a push whose body hash does not match a `sha256` the client
  states up front. Not because the registry is trusted — the consumer
  still verifies — but so corruption is caught at the door rather than
  by every puller afterwards.

**Storage should be content-addressed** (`blobs/<sha256>`), with the
published names as links into it. That gives deduplication for free
(the same binary reachable under several versions costs one copy),
makes `HEAD` exact, and makes garbage collection tractable.

**Auth asymmetry is deliberate**: pull may be open, because bytes are
verified by the consumer anyway; push must be authenticated, or anyone
can fill the disk or plant blobs that every puller then has to reject.

## 5. Deliberately out of scope

* **Recipes.** They come from git. See §2 — this is the one thing that
  must never be added, however convenient it looks.
* **Building.** The registry stores what builders produce; it never
  compiles. A registry that could build would need to be trusted.
* **Deciding what is current.** Version resolution belongs to the recipe
  catalogue (ADR-0107), not here.

## 6. Resolved: how a checksum reaches an existing recipe

Both tiers are now live. Five image recipes carry
`image_artifact_sha256`; all 66 package recipes carry
`pkg_artifact_sha256`.

The package tier posed a real question, and the answer constrains this
project. A package artifact's filename is `<name>-<version>.tar.gz`,
where the version is `pkg_version` — so bumping a recipe's version to
add its checksum *changes the URL it then looks for* and orphans the
artifact it was meant to describe. Retrofitting therefore required
editing recipes **in place** at their existing version.

That was done deliberately, at the operator's direction, on the
reasoning that recipe-version immutability exists so content cannot
change under something that already built from it — and adding a
checksum for byte-identical content records what was built rather than
changing it. `pkg.c` already carries a one-shot "replace exactly this
`name@version`" path (issue #59) for this class of correction.

**What this means for a push-capable registry:** retrofitting is a
migration, not a workflow. Once builders push automatically, a recipe
should carry its checksum from first publication and never be edited
again — which restores strict immutability. Design the push path so
that publishing a version and recording its checksum are one act, not
two, or this retrofit will have to be repeated every time.

## 7. Current contents of this directory

Produced by hand from a live host (192.168.15.95), and consumable as-is
by the URL shapes in §3:

* `images/` — 5 whole-rootfs artifacts, including `gcc-tcc-bootstrap`,
  which holds a `gcc 16.2.0-11` self-hosted from TCC with no external
  compiler anywhere in its lineage. That one represents days of compute.
* `packages/` — 66 individual package artifacts.
* `MANIFEST.json` — version, size and sha256 for every file above. The
  checksums here are a convenience for building recipes; they are **not**
  authoritative. The recipe in git is.

## 8. How these were extracted (useful history)

The host's own export endpoint could not be used: `tar -z` shells out to
a bare `gzip` through `PATH`, and the daemon runs as PID 1 with no
`PATH`, so every host-side compression fails (issue #125). Extraction
instead ran `tar` *inside a container* built from each image — where
`PATH` is normal — split the result into bounded chunks, and pulled each
chunk through `GET /containers/{name}/files`. Images with no shell or
archiver were handled by installing one first, or, where the packages
were small, by pulling each recorded file individually.

The lesson worth carrying: **a host cannot be assumed able to produce its
own artifacts.** A push-capable registry should tolerate artifacts
produced anywhere, by anything, and verify rather than assume.

## 9. Note on this file

This directory is currently `.gitignore`d in the Cix repository, because
it holds gigabytes of compiled output. This document therefore is not
under version control right now. **Commit it as the first act of the new
project**, before anything else is written.
