# 0008 — architecture in artifact names

## Status

Accepted. Closes itdlabs/cix-cache#3. Supersedes the "architecture is
deliberately not included" section of ADR-0007.

## Context

ADR-0007 decided not to put architecture in an artifact's name, on the
grounds that Cix is x86_64-only so the cache is right either way, and
that including it would rename every artifact for a dimension with one
value. That reasoning was about cost. It did not weigh what the failure
looks like, and the failure is the part that matters.

An artifact is fetched, checksum-verified, installed and **booted**.
Nothing in that path carries an architecture, because nothing in the
naming does. So the day an aarch64 host appears, an aarch64 build of
`tcc-0.9.27-7` either collides with the x86_64 artifact of the same name
— one overwriting the other, or the second push refused as a conflict —
or it lands and is then served to x86_64 hosts.

**A content checksum cannot catch this.** The artifact is not corrupt.
The bytes really are the bytes that were published, and they verify
perfectly. They are simply for a different machine. This is the one
failure the content-addressed design is structurally blind to, which
makes it the one that has to be prevented by naming instead.

## Decision

### 1. Architecture is part of identity

Canonical form is `<name>-<version>-<release>-<arch>`, with `arch`
spelled as `uname -m` spells it — the same spelling as
`cix-build-system`'s ADR-0001, because three repositories must not
invent three spellings of one machine. Recognition is a whitelist, so
an ordinary trailing word cannot accidentally become an architecture.

### 2. An architecture is never inferred, only asserted

Canonicalisation carries an architecture through when a name has one and
**never adds one when it does not**.

This is the load-bearing restraint. Storing a bare push as `-x86_64`
would attach a claim the pusher never made, and an aarch64 build pushed
under a bare name would then sit in the store under a false label —
strictly worse than no label, because a wrong statement about the bytes
is actionable and an absent one is not.

Stamping the existing store is therefore a separate command,
`--set-arch=x86_64`, taking the architecture as an argument. It is an
operator saying "everything already here was built for this", which is a
thing an operator can know and a parser cannot.

### 3. A bare name resolves only while it is unambiguous

A request carrying no architecture is matched against each known one and
resolves **only if exactly one** exists. If two do, it fails with 409
and names the problem.

This is what makes the migration free. Every recipe in git today derives
a bare URL, and 125 artifacts answer to one. Refusing those outright
would make every host silently rebuild from source — the invisible
failure of ADR-0005, at scale. Aliasing them to "the only architecture
present" keeps them working, and the alias evaporates by itself at
exactly the moment it would become dangerous: the second architecture's
arrival is what turns the bare name ambiguous.

So the guarantee is not "the cache always knows the right architecture".
It is narrower and honest: **the cache will never serve a name that
could mean two machines.** It fails instead.

### 4. Ambiguity is not a miss

A miss means "build from source" and is harmless. An ambiguous name is
neither harmless nor a miss: it is the store declining to guess. It
returns 409, is logged at error level, and is not counted as a miss —
counting it there would bury it in a number that is expected to be
non-zero.

## Consequences

Existing hosts need no change and no coordination with `itdlabs/cix`.
Old URLs keep working for as long as one architecture is published.

**The protection is not complete until the daemon asks for an
architecture.** While hosts send bare names, an aarch64 host asking for
a bare name on a single-architecture x86_64 store still gets x86_64
bytes. The cache cannot detect that — the request says nothing about the
asker, and there is no header that would. What this ADR buys is that the
failure becomes impossible the moment both architectures are published,
rather than becoming likely. Closing the remaining gap is `cix#179`:
once the daemon sends arch-qualified names, the bare alias should be
removed here, and that removal is the last step, not this one.

Deliberately not done: rejecting a push whose declared architecture
disagrees with its name. There is no declaration to compare against —
adding one is a protocol change, and the name is the load-bearing part.
