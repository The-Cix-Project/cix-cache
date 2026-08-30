# 0009 — how listings, manifests and versions are ordered

## Status

Accepted. Refines the listing behaviour assumed by ADR-0002 and the
manifest described in ADR-0001.

## Context

Artifact listings came out in `readdir()` order — whatever the
filesystem's hashing produced. Measured on the running store on
2026-08-29, across 124 rows, publish dates jumped between the 26th and
the 29th line by line. There was no question that order answered.

`MANIFEST.json` had the same problem, and worse. That file already
leaves out mtime precisely so that two identical stores produce
identical manifests — but `readdir()` order defeated the other half of
that intent, so the same store could generate two manifests that
differed only in key order.

The dashboard's Version column sorted by string collation, which gets
two things backwards: `v2.2.0-rc6` landed *after* `v2.2.0`, being the
longer string sharing its prefix, and `rc10` landed before `rc6`.

## Decision

### 1. Listings are newest published first

Most recently published first, ties broken on name.

The dashboard is search-first, so finding a known name is the search
box's job. That frees the list to answer the question an operator
actually has when they open it: what landed. It reads as a changelog.

Ordered by publish time and **not** by version, deliberately. Ordering
versions correctly needs the rules in section 3, and getting them
subtly wrong is the kind of thing that goes unnoticed for a long time.
The symlink's mtime is already recorded, already exposed, and already
what the Published column shows.

The tiebreak is not cosmetic. mtimes are whole seconds and a pushed
batch ties constantly; without a total order, equal keys leave rows
free to swap places between the dashboard's polls.

### 2. `MANIFEST.json` is ordered by name

A different document with a different job: it exists to be compared
against another copy of itself, and a chronological manifest diffs as
noise. Sorted by name, it is byte-identical for identical stores, which
is what leaving mtime out was always trying to achieve.

### 3. Versions compare by rule, not by collation

Upstream versions are taken verbatim and are not semver — `10.4p1`,
`s20180629`, `1.5.8.pl02` and `v2.2.0-rc6` are all in this store. So
the rules are written for what is actually published:

1. a leading `v` before a digit is ignored, so `v2.1.1` and `2.1.1` are
   one version;
2. the string splits at the first `-` into version and prerelease, and
   a version **with** a prerelease is older than the same version
   without one;
3. each side compares run by run — a run being consecutive digits or
   consecutive non-digits — with digit runs compared as numbers, so
   `2.1.10` follows `2.1.8`;
4. a digit run outranks a text run in the same position;
5. running out first is older.

Rule 5 carries more than it looks. It makes `1.2` older than `1.2.1`,
and — deliberately — `10.4` older than `10.4p1` and `1.5.8` older than
`1.5.8.pl02`. Those two are in this store and are **patch levels, not
prereleases**: they are newer than the version they patch. Only a
hyphen introduces a prerelease, which is what keeps them out of rule 2.

A separator is its own run. Without that it is swallowed by the text
after it — `.beta` compares as one token against `.` — and rule 4 never
fires. That was a real bug, caught by a test rather than by reading.

These rules cannot order every string anyone might publish. Nothing
can, when the input is "whatever upstream called it". The intent is to
be predictable and written down rather than clever.

### 4. The comparison lives in the store, and the browser gets a number

`/api/v1/artifacts` reports `version_rank`, each artifact's position
under `store_version_cmp()`. The dashboard sorts on that integer.

One implementation of the rules. A second comparator in JavaScript
would eventually disagree with the server about which artifact is
newer, and only one of the two would be the one anything acts on. It
also puts the logic where the test harness can hold it — there is no
JavaScript one, and untested ordering logic is exactly how this class
of bug survives, because it is quiet and plausible-looking and nothing
fails.

### 5. Columns are sortable, and the default stays newest

Newest-first answers "what landed", which is right as a default. It is
the wrong order for the other two questions a long list attracts —
scanning for a name, and finding what is taking up the room. Both are
one click.

Direction depends on the column: text opens A–Z, while size, release
and date open largest or newest first, because that is the reason
anyone clicks them. Ties break on name, for the same reason the
server's ordering does.

## Consequences

Sorting is done once, in the store, so every consumer of the API sees
one order and there is one place the policy is written down.

`store_rank_versions()` sorts pointers rather than copies so the rank
writes straight back through them; sorting copies would need the
originals found again by name afterwards, which is a quadratic scan on
a listing that only gets longer.

The version column's ordering is presentation. Nothing resolves an
artifact by comparing versions — the store is a flat map from an exact
name to a digest — so a wrong comparison here cannot serve wrong bytes.
That bound is worth keeping: it stops holding the moment anything
*chooses* between versions, which is what a "latest" endpoint or
retention-by-version would do. Neither exists, and neither should be
built on collation.
