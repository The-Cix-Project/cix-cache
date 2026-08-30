# 0003 — push protocol, and what a published name may mean

## Status

Accepted. The image-tier PUT/HEAD paths described here were removed
in v2.0.0 -- see ADR-0006. The protocol, the auth asymmetry and the
immutability rule are unchanged for the package tier.

## Context

Cix shipped the *consuming* half of the artifact tier and nothing that
produces or serves it. Every artifact in this store was extracted by hand,
because the host's own export endpoint cannot work: its `tar -z` shells
out to a bare `gzip` through `PATH`, and the daemon runs as PID 1 with no
`PATH` (issue #125 in the Cix repository), so every host-side compression
fails. Extraction instead ran `tar` inside a container built from each
image, split the output into bounded chunks, and pulled each chunk through
`GET /containers/{name}/files`.

The lesson recorded in DESIGN.md section 8 is that **a host cannot be
assumed able to produce its own artifacts**, so a push-capable registry
must tolerate artifacts produced anywhere, by anything, and verify rather
than assume.

DESIGN.md section 6 adds a constraint that is easy to miss. Retrofitting
`pkg_artifact_sha256` into existing recipes required editing them *in
place* at their existing version, because a package artifact's filename
contains `pkg_version` — so bumping the version to add a checksum changes
the URL the recipe then looks for and orphans the artifact it was meant to
describe. That retrofit was a migration, not a workflow. The design
document asks that publishing a version and recording its checksum be
**one act**, or the retrofit will have to be repeated forever.

## Decision

### Wire protocol

```
PUT <base>/<name>-<version>.tar.gz
PUT <base>/images/<name>-<hash>.tar.gz
HEAD on either
DELETE on either
```

A push must declare its digest up front in `X-Cix-Sha256` (64 lowercase
hex) and must send `Content-Length`. The body streams to `tmp/`, is hashed
there, and is only then adopted into `blobs/` and published.

A custom header rather than RFC 9530 `Repr-Digest`: the parser here is
hand-rolled and `http_find_header()` already does exact-length
case-insensitive matching, so a plain hex header costs nothing to read and
is greppable in a log. Structured-field parsing would be the only thing in
this codebase that needed it.

**Rejecting a mismatch is not a trust claim.** The consumer still verifies
every byte against its recipe; this server remains no part of that chain
(ADR-0002). The check exists so corruption is caught at the door instead
of by every puller afterwards.

### Authentication is deliberately asymmetric

Pull may be open. Push may not.

Bytes served are verified by the consumer against a checksum from git, so
an open pull surface gives an attacker nothing — they cannot make a host
accept anything a recipe does not already approve. An open *push* surface
lets anyone fill the disk, or plant blobs that every puller then has to
fetch and reject. So `PUT`, `DELETE`, `POST /api/v1/gc` and
`POST /api/v1/import` require a bearer token; `GET` and `HEAD` require one
only if `pull_token` is configured.

The dashboard shows `push: OPEN` in a warning colour when no push token is
set, because that is the one misconfiguration worth shouting about.

### A published name may only ever mean one byte sequence

Publishing a name that already resolves to a **different** digest returns
**409 Conflict**. Publishing one that resolves to the **same** digest is
an idempotent success.

This is the enforcement of "publishing a version and recording its
checksum are one act". A recipe version is immutable in git; if the bytes
under its name could be replaced, an artifact could drift out from under a
recipe that already built against it, and the checksum in git would start
failing for reasons no one could see from the recipe. Refusing the second,
different act is what keeps a name and a digest welded together.

A rejected push leaves its blob in the store, unreferenced. That is
intended: the alternative is deleting bytes that some other name may
legitimately share. The collector reclaims it (ADR-0001).

`DELETE` unpublishes a **name** only. Blobs are removed solely by garbage
collection.

### Hashing

Forked to `/usr/bin/sha256sum` by absolute path — never hand-rolled, and
never through `PATH`. Both halves matter. The first is the rule already
governing every checksum in the Cix codebase (its ADR-0108: "never
hand-rolled crypto"). The second is the direct lesson of issue #125: a
`PATH` lookup is exactly what fails when a server runs as PID 1, which is
how the artifacts in this very store came to need extracting by hand.

Hashing a 2.7 GB upload takes on the order of ten seconds, far too long to
block a single-threaded reactor, so the child is watched as a `pidfd`
registered in epoll rather than waited on. See ADR-0004.

### A refused push is refused before its body is adopted

Amended 2026-08-30 (#7). The conflict was originally discovered at
publish time, after the body had been hashed and adopted into `blobs/`
— so a refusal left the rejected artifact on disk with nothing pointing
at it until a collection.

The name is now resolved between hashing and adopting: if it is taken
by different bytes the temp file is unlinked and nothing reaches
`blobs/`. `store_publish()` still makes the authoritative decision;
this only settles whether the body becomes a blob or is discarded.

Worth recording what the fix did *not* turn out to be. The cost was one
wasted copy per **distinct** refused artifact, not one per attempt:
adopting is content-addressed, so a retry of the same rejected bytes
deduplicates onto the orphan already there. A first regression test
passed against the buggy code for exactly that reason.

An ambiguous name (ADR-0008) is likewise a refusal and not a fault, and
returns 409 rather than the 500 it originally produced.

## Consequences

- A builder can `HEAD` before uploading and skip pushing hundreds of MB it
  does not need to. The Cix daemon never sends `HEAD` itself; this exists
  purely for producers.
- Correcting a genuinely wrong artifact is deliberately a two-step
  operation — `DELETE` the name, then `PUT` the new bytes. There is no
  single call that silently replaces content under a live name.
- The store never trusts a client's declared digest as fact: it is
  compared against a hash computed here, over the bytes as received.
- Uploads are capped (`UPLOAD_MAX_BYTES`, 16 GiB) so a `Content-Length`
  alone cannot commit the disk.
