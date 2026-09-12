# 0013 — the daemon wears the grammar it enforces

## Status

Accepted. Refines ADR-0007 and ADR-0008 by applying them to the server
itself rather than only to what it stores.

## Context

`cixcached` reported its build from `git describe --tags --always
--dirty`:

```
v2.17.1-7-g00f0b63
```

Every artifact this store holds is named
`<name>-<version>-<release>-<arch>` — upstream's version, Cix's
packaging revision of it, and the machine it was built for. That
grammar is enforced on push, canonicalised on mismatch, and refused
outright when it does not fit.

Fed through the store's own parser, the daemon's version came back as
one opaque version string with **no release and no architecture**:

```
cix-cache-v2.17.1-7-g00f0b63  ->  name=cix-cache  version=v2.17.1-7-g00f0b63  release=1  arch=(none)
```

against, for a package it actually serves:

```
cix-v2.57.100-1-x86_64        ->  name=cix        version=v2.57.100           release=1  arch=x86_64
```

So the daemon was the one thing in the system not describable by the
rules it imposes on everything else. An unstamped artifact is something
ADR-0008 treats as dangerous enough to count and warn about on the
dashboard; the server reporting itself that way was not noticed because
nothing parses its version.

## Decision

The build identity is a canonical artifact name.

```
cix-cache-v2.18.0-1-x86_64
```

* **version** — the release tag, verbatim, the way a package carries
  upstream's version.
* **release** — the packaging revision *of that version*: 1 at the tag,
  and one more for each commit past it. That is what a release number
  already means for a package whose upstream has not moved, so a build
  seven commits past `v2.17.1` is `v2.17.1-8`, not a hash.
* **arch** — `uname -m`, spelled as the store spells it.

A dirty tree is **not** folded into the name. It is not part of an
artifact's identity, and bending it in would put `dirty` exactly where
the architecture goes — the mis-stamping ADR-0008 exists to prevent,
done to ourselves. It is reported as its own fact: `build_dirty` in
`/api/v1/status`, a marker on `--version` and in the startup log, and
the version rendered in the dashboard's warning colour.

`test_store` asserts the generated identity is a name this store would
accept, splits into all four fields, and carries no abbreviated commit.
The claim is checked by the parser rather than trusted from the
Makefile that produces it.

## Consequences

The commit is no longer in the version string. It was the one thing
`git describe` gave that this does not, and it was reachable only by
someone with the repository — who can get it from the tag and the
release number, which is the same information in the project's own
vocabulary. `build_time` remains, and a dirty build now says so
explicitly rather than by a `-dirty` suffix nobody reads.

This only reads as a version at all because the project tags releases.
An untagged repository reports `v0.0.0` with the commit count as its
release, which is honest and obviously not a release.
