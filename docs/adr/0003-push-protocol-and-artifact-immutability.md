# 0003 — push protocol, and what a published name may mean

## Status

Accepted. The image-tier PUT/HEAD paths described here were removed
in v2.0.0 -- see ADR-0006. The protocol, the auth asymmetry and the
immutability rule are unchanged for the package tier. Amended by #7,
and by #24: refusals are decided before the body, `Expect:
100-continue` is answered, and a refused connection half-closes instead
of resetting.

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

### Amendment, #24: a refusal is decided before the body, and heard

The server answered a refused push before reading its body, then closed
the socket with that body still unread. Closing with unread bytes makes
the kernel send RST, and an RST discards any response the client has
not read yet. curl, still uploading, saw no status at all for about 4%
of refused 200 KB pushes (8 of 200, measured 2026-11-22). That was the
`test_push` flake. Over HTTPS a failed write ends the session, so for
any body larger than the socket buffers the answer was always lost.
Three changes:

- **Everything that can refuse a push is checked before the body.**
  This adds the immutability conflict, checked against the declared
  `X-Cix-Sha256` through the same `refuse_if_taken()` that guards
  adoption, so a body that matches its declaration gets the same
  answer from both. A body that does not match was refused anyway, and
  now as 409 rather than 400. The check after hashing stays, and is
  still the authoritative one.
- **`Expect: 100-continue` is answered.** `100 Continue` is sent only
  once every check has passed and no body has arrived; a refusal is
  sent instead of it. curl adds the header by itself for bodies over
  1 MB and waited a second for nothing on every large publish (a 5 MB
  push went from 1037 ms to 23 ms). `cix cache publish` asks for it,
  and against a server that never answers it sends the body after a
  second, as curl does.
- **A response that leaves a body unread half-closes.** The server
  sends close_notify under TLS and `shutdown(SHUT_WR)`, then reads and
  discards the rest of the body until it ends or the client closes.
  The bytes are never staged and never parsed. As first written, this
  also stopped after 4 MiB or 5 seconds; #25 and #26 changed that (see
  below).

### Amendment: refusals of any size and length (#25, #26)

#24 left two ways for a refusal to still end in a reset. Both affected
a client that writes its whole request before reading anything, such as
Python's `http.client` or a hand-written client. curl and `cix cache
publish` were never affected: they ask with 100-continue, and curl also
reads while it sends. Measured on e5c35b3, 10 runs per row:

| refused upload, sent in full before reading | answer received |
|---|---|
| `Content-Length`, 8 MB or 30 MB | 0 of 10 |
| chunked, 2 MB | 3 of 10 |
| chunked, 3 MB or more | 0 of 10 |

- **The drain is limited by time, not bytes** (#26): 30 seconds in all,
  and 5 seconds without a byte. These are nginx's lingering-close
  defaults (`lingering_time`, `lingering_timeout`). A 4 MiB cap had
  turned every larger refused body into a reset. Discarding costs a read
  into a stack buffer. Each wakeup discards at most 1 MiB, and epoll is
  level-triggered, so the rest waits for the next pass rather than
  holding the reactor. With three 1 GB refused uploads in flight, the
  slowest of 20 status requests took 6 ms, against 55 ms without that
  limit. A refused body that takes more than 30 seconds to send still
  loses its answer, unless the client asked with 100-continue.
- **A refusal whose length is unknown drains too** (#25): a chunked
  request, which is always refused because an upload needs a
  `Content-Length`, and a request refused before it could be parsed
  (headers too large, an unreadable length). The server drains it until
  the client closes, within the same limits. A request with no body and
  no length still closes at once, as before.
- **An upload with both `Content-Length` and `Transfer-Encoding` is
  refused with 411**, even with the token. The two headers disagree
  about where the body ends, which is how request smuggling works.
  Before this, the server took `Content-Length` and treated the chunk
  framing as body bytes.

After both, every row in the table above answers 10 of 10, over plain
HTTP and over HTTPS. `test_push` checks a 16 MiB body sent in full, a
chunked upload, an upload with both headers, a malformed request, and
a quiet client being let go after 5 seconds. The 16 MiB, chunked and
malformed checks fail on e5c35b3, and the last fails when the 5-second
limit is removed.

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
