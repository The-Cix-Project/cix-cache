# 0012 — CIXPKG is a package format, and the signature rule is policy

## Status

Accepted. Closes itdlabs/cix-cache#13, #14 and #15. Refines ADR-0006
(what a package is), ADR-0009 (what `version_rank` orders) and ADR-0010
(what being signed means), and reverses nothing.

## Context

Cix intends to adopt CIXPKG as its package format in place of `.tar.gz`
(cix-build-system#141). CBS builds and verifies the format today —
measured on a Cix host, 2026-09-11, building `recipes/zstd.cbs` from a
pre-populated cache with no network: `zstd.cixpkg` at 306,483 bytes,
identity `zstd-1.5.4-1-x86_64`, symlinks preserved through extraction,
and a second independent build byte-identical by sha256. So this is a
real format with deterministic output, not a plan.

The store was the prerequisite, and it did not recognise the suffix.
Measured against the store at HEAD on 2026-09-11, the two errors being
the discriminator, so no write was needed to establish it:

```
doesnotexist-1-1-x86_64.tar.gz   404 {"error":"not found"}
doesnotexist-1-1-x86_64.cixpkg   404 {"error":"no such endpoint"}
```

A recognised name that is absent reports `not found`. `.cixpkg`
reported `no such endpoint` — it was not an artifact name to this
server at all, so the router never reached the store.

That is one table entry. What needed deciding was everything that
followed from an artifact being able to exist in **two encodings at
once**, which is the whole shape of a migration rather than a switch.

## Decision

### 1. `.cixpkg` and `.cixpkg.minisig` are recognised suffixes

Compound before base in the suffix table, per the ordering rule the
table already stated: a compound suffix must win over the shorter one
it ends with.

Everything else followed from the table with no code of its own, which
is the evidence the table was the right place to change. A `.cixpkg`
parses to the same name, version, release and architecture as its
`.tar.gz` counterpart, canonicalises the same way, and its detached
signature composes exactly as ADR-0010's does.

`.cixpkg` is typed `application/octet-stream`. CIXPKG is its own
container with its own header, not a gzip stream under another name, so
`application/gzip` would be a lie to anything that believes the header.
There is no registered type; this is stated as a case rather than
reached by falling off the end of the function.

### 2. The suffix table is the only place a suffix's properties live

One table, carrying for each suffix what tier it belongs to and whether
it may be published unsigned. Both of those were previously answered by
comparing against `".iso"` at each site that asked — which is how the
two questions came to be the same function, and how the next site to
ask would have got one of them wrong.

### 3. The listing groups by identity

`GET /api/v1/artifacts` returns one record per **stem**, carrying a
`formats` array.

As flat rows, two encodings of one artifact shared `artifact`,
`version`, `release`, `arch` **and `version_rank`**, differing only in
`name`, `sha256` and `bytes`. `version_rank` exists precisely to order
by identity (ADR-0009), so a consumer grouping on it saw two rows
fighting for one position. Grouped, it identifies a row again.

The stem is the group key because the stem is the identity: the parsed
fields are split for display only, and two encodings share the stem by
construction.

What differs per encoding stays inside a format — `format`, `name`,
`sha256`, `bytes`, `signed`. There is deliberately **no
identity-level `sha256`**: one digest standing for two different blobs
would be a lie, and a client that wants a digest wants a format's.
`bytes` and `modified` are reported per identity because they aggregate
honestly — what the artifact costs on disk, and when it last landed.

`count` is now records, so `files` carries the number it used to mean.
Both are reported because they stopped being the same fact, and a
consumer reading `count` as a file count would be quietly wrong rather
than visibly broken.

Ranking is over identities, and the representative carries the stem
rather than a filename. Not cosmetic: the comparator's final tiebreak
is the name string, so ranking on filenames would let publishing a
`.cixpkg` beside a `.tar.gz` move an **unrelated** artifact's rank.

Scope is that endpoint alone. `MANIFEST.json` and `/api/v1/status` are
the daemon-facing contract and count files, ungrouped — two encodings
of one artifact really are two files on disk, and that is what an
operator asking about disk wants to know.

**Amended by #22: `MANIFEST.json` groups too.** The scoping above holds
for `/api/v1/status`, which *counts* — a count of files is a fact about
disk and stays ungrouped. It could not hold for `MANIFEST.json`, and
the reason is structural rather than a matter of taste: that file is a
JSON **object keyed by identity**, so "one entry per file, ungrouped"
and "one key per identity" cannot both be true. What it actually
produced, the first time a store held two encodings of one identity,
was the same key written twice with different `file`, `sha256` and
`bytes` — a document with no single meaning, since duplicate names are
implementation-defined in RFC 8259 and this repo's own reader takes the
first while `jq` and Python take the last.

So a `packages` or `installers` entry now carries a `formats` array,
one element per encoding, in the same vocabulary section 3 gave the
listing. Grouping was never optional here; only its absence was.

The other premise is worth correcting while it is being touched: this
section calls `MANIFEST.json` "the daemon-facing contract". ADR-0002 is
narrower and disagrees — the daemon "never requests an index", and this
file is "a convenience for humans writing recipes" whose checksums are
explicitly not authoritative. Nothing on an install path reads it, and
that is precisely why changing its shape was affordable.

### 4. The signature requirement is configuration, defaulting to today

`require_signature`, a list of suffixes that may not be published
unsigned. Default `.iso`, which is the historical behaviour.

Not a hard mandate for packages, because the argument for one is weaker
than it looks: **the store is explicitly not a trust boundary**
(ADR-0002 here, and cix-build-system ADR-0001 — "the artifact server
supplies bytes and is not a trust boundary"). What approves an
installed package is the `pkg_artifact_sha256` in its git recipe,
fetched from a different host over a different protocol, and cix
ADR-0279's install-side check. Refusing an unsigned upload is hygiene
on top of that, not security, and a store-side mandate does not replace
the install-side gate.

Not left alone either, because the ISO rule then sits in the code as a
single `strcmp` with no reason beside it, and because the balance moves
once CIXPKG is what Cix installs. Configuration is what lets an
operator who has finished rolling out signing keys tighten the rule
without the store deciding for them.

The cost of the mandate is what keeps it off by default: it turns an
accept into a 409 for any Cix host whose signing key is not configured,
which during coexistence is most of them.

An unrecognised suffix is reported on stderr at startup and not
enforced, rather than being fatal. Refusing to start takes the cache
down, and a cache that is down means every host builds from source —
slow, but safe by ADR-0005's reasoning, and still an outage nobody
asked for over a typo. The warning is loud because the failure it
describes is silent in the dangerous direction: not enforcing what an
operator asked for.

### 5. The tier is not configurable, and is a separate question

`store_is_installer()` answers the tier. `store_needs_signature()`
answers the policy. They were one function, and separating them was a
precondition for item 4 rather than a tidy-up.

While they were one function, turning the requirement on for `.cixpkg`
would have moved every `.cixpkg` **out of `MANIFEST.json`'s `packages`
section** — the section a package is looked up in by `name@version` —
and into `installers`, which describes bootables. A configuration key
would have quietly moved the store's whole reason to exist into the
section nobody looks for a package in. It
would also have counted every package as a bootable installer in
`/api/v1/status` and the dashboard.

So the tier is a fixed column of the suffix table that configuration
cannot write, and there is a test in the manifest's own suite asserting
that a `.cixpkg` under a package-covering policy is still a package.

## Consequences

A signed artifact reports `signed` and an unsigned one omits the field
rather than reporting `false` — the rule ADR-0010 set, now asked per
encoding since each carries its own detached signature. One format of
an artifact may be signed while the other is not, and the listing says
so.

Changing `require_signature` changes when `signed` is **reported**, not
only when a push is refused: the field appears when an encoding
requires a signature or carries one. That is correct — under a policy
requiring them, an unsigned artifact that predates the policy should
read as unsigned rather than as not-applicable — but it means tightening
the policy makes existing artifacts start reporting `"signed": false`.

`config` is an overlay: a key a config file does not mention keeps its
default. It did not behave that way — every unmentioned key came back
empty, so `conf_defaults()` was the source of truth only for a config
file that did not exist. That was a latent bug, and it became a
dangerous one the moment a default carried a signature policy: an
existing deployment's config does not mention `require_signature`, and
blanking it would have turned the ISO rule off everywhere on upgrade.

A store may now hold two encodings of one identity, and nothing
collects one in favour of the other. `store_gc()` is a blob-orphan
collector — it builds the live set from every published entry and
removes unreferenced blobs — not a "keep N versions" pruner, so two
live entries for one identity are simply two live entries. Recorded so
nobody re-derives the fear.

Legacy non-canonical spellings coexisting with canonical ones form
separate groups in the listing, because they are separate names on
disk. That is truthful rather than desirable, and
`cixcached --canonicalize` is still how it is resolved.
