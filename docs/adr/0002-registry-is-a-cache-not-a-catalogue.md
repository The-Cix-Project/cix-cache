# 0002 — the registry is a cache, never a catalogue

## Status

Accepted. The image rows in the wire-contract table below were
removed in v2.0.0 -- see ADR-0006. The invariant itself is unchanged
and ADR-0006 follows from it.

## Context

Cix deliberately splits recipes and binaries across two independent URLs
(ADR-0122 and ADR-0123 in the Cix repository), and this is a security
property rather than a filing convention:

1. the recipe comes from git — versioned, reviewable, diffable text;
2. the artifact comes from this registry — opaque bytes;
3. the recipe's own `pkg_artifact_sha256` / `image_artifact_sha256`
   validates the artifact.

Therefore this registry **is never a trust boundary**. It does not decide
what is legitimate; the recipe in git does. A registry that also served
the recipes would be vouching for its own payload, which is worth nothing.

DESIGN.md section 2 records that this has already been violated once: the
image recipes were first generated *into this directory*, next to the
binaries they validate. They now live in the Cix repository under
`recipes/image/*`. The design document predicts the mistake will recur
under time pressure and asks that the system be designed against it.

A weaker invariant follows from the first, and is the one this ADR exists
to defend:

> The registry is a cache, never a catalogue. Git is the record of what
> exists. If anything ever has to consult the registry to answer "does
> this package exist", the split has been broken.

Building a web dashboard puts that directly in tension, because a
dashboard is literally a catalogue view. So does any listing API.

## Decision

Keep the listing, and draw the line at who may consult it.

**`/api/v1/*` is operator-facing observability only.** No Cix host code
path may ever consult it. This is a hard invariant, not a guideline.

The machine-facing surface is exactly:

| Purpose | Request |
|---|---|
| package artifact | `GET <base>/<name>-<version>.tar.gz` |
| image artifact | `GET <base>/images/<name>-<hash>.tar.gz` |
| existence probe (push clients only) | `HEAD` on either |
| publish | `PUT` on either (ADR-0003) |

Nothing in that list requires enumeration, and nothing in it answers "what
exists" in general — only "do you have this exact thing", asked by
something that already knew the exact name from a recipe in git.

This is enforceable by observation rather than by hope: the Cix daemon
only ever runs `curl -fsSL [-H "Authorization: Bearer …"] -o <path> <url>`
against a base URL. It never sends `HEAD`, never a `Range`, and never
requests an index. If a future change to the daemon needs a listing to
decide what to install, that change is what is wrong, not this ADR.

The dashboard says so in its own copy, next to the table, so an operator
reading the listing understands what it is and is not.

`MANIFEST.json` is generated and served for the same reason and under the
same restriction: it is a convenience for humans writing recipes, and
DESIGN.md section 7 already states that its checksums are **not**
authoritative. The recipe in git is.

## Consequences

- A 404 is an ordinary, cheap answer and is never logged as an error. It
  means "build it from source", which is the daemon's documented
  behaviour on any status >= 400.
- The server holds no database, no index and no catalogue file. Every
  answer the API gives is derived from the filesystem at request time, so
  there is nothing that can disagree with the tree.
- The registry can be deleted entirely and rebuilt from git plus compute.
  That must stay true; anything that would make the store the only record
  of something is out of scope by construction.
- Recipes must never be served from here, however convenient it looks.
  This is the one thing that can silently destroy the security model, and
  it is the mistake most likely to be repeated.
