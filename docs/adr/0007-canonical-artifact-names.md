# 0007 — canonical artifact names, and aliasing the old ones

## Status

Accepted. Closes itdlabs/cix-cache#2. Refines the naming half of
ADR-0001 and the immutability rule in ADR-0003.

## Context

A package's identity is `<name>-<version>-<release>`. `version` is
upstream's, verbatim; `release` is Cix's own packaging revision *of*
that upstream version, and moves when the recipe changes and upstream
does not.

The convention was real but unenforced, and the cache is where naming
becomes observable, so it is where the drift shows. Counted on the
running store on 2026-08-28: **89 published artifacts, 59 carrying a
release suffix and 30 not.** A third of the store was non-canonical.

That matters because of what two spellings of one thing cost. If both
`mtools-4.0.49` and `mtools-4.0.49-1` can exist, they are the same
bytes under two names: two entries, two checksums for a reviewer to
approve, and a real chance of a host installing one while a recipe
approves the other. The load-bearing rule is not the number — it is
**one package, one canonical name.**

### Why release defaults to 1, not 0

Because the repository already behaves that way and always has. Counted
in `recipes/package/`: 43 recipe versions end in `-2`, and exactly two
end in `-1`. Every package that has been revised went bare → `-2`,
skipping `-1` entirely. That only makes sense if a bare version is
*already understood* as release 1 and the first suffixed revision is the
second packaging. Defaulting to 0 would contradict the existing corpus
and make every `-2` mean "the third packaging". It also matches RPM and
Debian, where a first packaging is `-1` and `-0` signals something not
yet released.

## Decision

### 1. The parsing rule is narrow, and only looks at the tail

The release is the last hyphen-separated component when **it is all
digits, and the component before it contains a digit.**

Nothing before that tail is examined, because it does not need to be.
The name/version boundary cannot be found reliably at all —
`openldap-client-2.6.14` and `nss-pam-ldapd-0.9.13-2` put hyphens on
both sides of it — and canonical form only asks whether a release is
already present.

The second half of the rule is what keeps a date-style version intact.
In `foo-20250101` there is no version for a release to be a release
*of*, so the digits are the version, not release 20250101 of a package
called `foo`. **A release only exists relative to a version.** The same
reasoning means a name carrying no version at all is left alone
entirely: appending `-1` to `noversion` yields `noversion-1`, which then
reads as version 1 and would canonicalize again — canonical form has to
be a fixed point, or an entry stored under one spelling becomes
unreachable under the other.

An explicit release is re-rendered without leading zeros, so `-007` and
`-7` cannot both be names for one thing.

This rule cannot decide one case: an upstream version that itself ends
in `-<digits>` on a package carrying no release. Nothing in a flat
filename can. So the rule is *stated* rather than inferred — a trailing
all-digit component is the release — and the ambiguity is documented
instead of being discovered later.

### 2. A non-canonical name is an ALIAS, not a 404 and not a redirect

This is the decision the ticket asked to be made explicitly, and the
answer is a third option it did not list.

Canonicalization happens in `entry_path()`, the single choke point every
store operation already passes through. So a request for
`mtools-4.0.49.tar.gz` and one for `mtools-4.0.49-1.tar.gz` reach the
same file. **Exactly one entry exists on disk** — one checksum, one
listing row, nothing to drift — and both spellings resolve to it.

A 404 was the obvious alternative and is the wrong answer. Recipes in
git derive their URL from the recipe's own version field, and 30 of them
still spell it the old way. Returning 404 would make every one of those
hosts silently rebuild from source — the exact invisible failure ADR-0005
exists to prevent, at scale, and triggered deliberately.

A redirect is worse: the Cix daemon will not carry its bearer token
across a cross-host redirect, and it buys a round trip for nothing.

Doing this at one choke point rather than per caller is also what stops
resolve, publish and unpublish from ever disagreeing about which file a
name means — a disagreement that would surface as a 409 against a name
that appears not to exist.

### 3. Aliases are counted and logged

An alias hit works, which is precisely why nothing would otherwise ever
mention it. So each one logs a line naming both spellings, and
`artifact_aliases` is reported in `/api/v1/status` and by `cixcachectl
status`. That count is the only signal that a recipe somewhere still
uses the old name — it is a migration progress bar, and it should reach
zero.

Same reasoning as ADR-0005: a thing that silently works is not
observable, and unobservable is how the layout bug survived.

### 4. Migration renames symlinks only

`cixcached --canonicalize [--dry-run]` renames non-canonical entries.
Published names are symlinks, so **no blob is touched and no bytes
move** — the migration cannot lose data, and its cost does not scale
with the size of the store. A rename whose target already exists is
refused and counted rather than clobbering: two names collapsing to one
canonical name is a question only an operator can answer. On this store
there were none — 89 names mapped to 89 distinct canonical keys.

It is offline and explicit, like `--import`. A daemon should not migrate
an operator's data on its way up.

### 5. `release` is its own field

`/api/v1/artifacts` reports `artifact`, `version` and `release`
separately, and `version` is upstream's alone. Consumers do not re-parse
the string, and the dashboard and `cixcachectl ls` each gained a column
— without which `bash 5.2.37-1` and `bash 5.2.37-2` render as two
identical rows.

## Consequences

The migration is invisible to every existing host: old URLs keep
working, unchanged, indefinitely. There is no flag day and no
coordination with the `cix` repository, which is what makes it safe to
do at all.

The alias is permanent, not a deprecation window. Removing it later
would break exactly the recipes it protects, and would buy nothing —
it costs one string comparison per request and no storage.

`--canonicalize` **must be run once** when upgrading past v2.3.0. From
that point the server looks for canonical names, so an unmigrated entry
stays listed but stops resolving. This is the one upgrade step that is
not automatic, and it is why the flag prints what it would do first.

### Architecture is deliberately not included

`cix-build-system`'s ADR-0001 specifies identity as
`gcc-16.2.0-11-x86_64`, including architecture. Cix is x86_64-only
today, so the cache is right either way, and the decision was made
rather than deferred: **not included.** Adding it would rename all 89
artifacts and every recipe-derived URL for a dimension that has exactly
one value.

Adding it later is cheap by the same alias mechanism that made this
migration cheap. It is not free, though, and the cost is recorded here
so it is not a surprise: `gcc-16.2.0-11-x86_64` parses under the current
rule as having no release, because `x86_64` is not all digits. The
parser would need to recognise a trailing architecture from a small
allowlist before reading the release. That is a contained change to
`release_offset()`, and it is the only thing standing in the way.
