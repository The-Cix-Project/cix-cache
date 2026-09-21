# 0010 — installer ISOs are served, and signed

## Status

Accepted. Closes itdlabs/cix-cache#8. Refines ADR-0006 rather than
reversing it, and narrows ADR-0002 for exactly one class of artifact.

Section 4's "deletes are not coupled" is amended by #21: a delete now
removes an artifact's detached signature with it. Deleting a signature
on its own is still allowed, which was the half of that decision the
measurement did not touch. Section 5 is unchanged, but its last
paragraph no longer rests on section 4.

## Context

ADR-0006 removed whole-image artifacts. The argument was that an image
is a recipe composed of packages, its version is the hash of its own
package manifest, and **a running Cix host can compose it itself** — so
storing one duplicated bytes the store already held, 3.85 GB of a 4.5 GB
store at the time.

Every step of that depends on there being a host to do the composing.

An ISO is what you boot to *create* a host. On the far side there is
bare metal, or an empty VM, and a person with a USB stick — nothing
that can resolve a recipe or fetch a package, because none of that
exists until the ISO has already run. The alternative to caching an ISO
is not "compose it locally"; it is "build it by hand, or copy it around
out of band".

So ADR-0006's reasoning does not reach ISOs, and the distinction is
worth writing down, because "packages are the only tier" is otherwise
the obvious thing to cite against this.

## The problem an ISO creates

Everything else here is protected by a checksum that arrives by a
different route than the bytes: a package is approved by a
`pkg_artifact_sha256` in a git recipe, fetched from a different host
over a different protocol. That is what lets ADR-0002 say the registry
is never a trust boundary, and it is why a compromised cache buys an
attacker nothing today — serving evil bytes just makes hosts refuse
them.

An ISO has no recipe behind it. It is fetched by a person and then
**booted**, which is the most privileged thing any artifact in this
system does. Served with a checksum that comes from the cache itself,
cache compromise would become root on every machine installed
afterwards — weakening the guarantee precisely for the artifact that
deserves it most.

Rejected, with reasons, so they are not relitigated:

- **Checksum in git.** Git works for artifacts whose identity *is* a
  commit. An ISO regenerable on any afternoon is not one, and
  committing a checksum per build is ceremony that rots.
- **The pusher declares it.** Already true — `X-Cix-Sha256` is required
  on every `PUT` and comes back from `HEAD`, the API and the manifest.
  But all of those are the cache. A checksum means something only if it
  reaches the verifier by a path the bytes did not.
- **Accept the cache in the trust domain.** Legitimate for one
  operator, but see above: it inverts the risk.

## Decision

### 1. Sign the digest at generation

The build box signs; the cache stores the ISO and a detached signature;
a verifier needs only the public key, published once and thereafter
effectively never. That decouples the per-ISO part — checksum and
signature, both safe to serve from the cache, which cannot forge one —
from the trust anchor, which is stable and comes from elsewhere.
Nothing per-ISO goes into git, which is what made git wrong.

**minisign format.** Signed with OpenSSL's Ed25519, since the daemon
already links it and no new dependency is wanted on the build box;
verified with upstream `minisign`. The deciding argument was not size:
**the verifier can be a tool we did not write.** This is the artifact
that gets booted, and the check is the last thing between a substituted
ISO and a machine. Only the verify side is a trust boundary, which is
what makes the asymmetric implementation right rather than a smell.
minisign also carries no expiry, no revocation and no certificate
chain — X.509 would need all three deliberately switched off, and each
is something that can get switched back on by accident.

### 2. Verification happens before boot

An installer validating its own signature is the code being checked
doing the checking: a substituted ISO reports success, or omits the
check. The same applies to a persistent warning on the installed
system, since the installer wrote that system.

Verification belongs on an already-trusted machine, **before the USB is
written**. That is the only ordering in which a failed check can still
stop anything. An in-installer self-check is still worth having, but
its honest job is corruption — a bad flash write, a dying stick — and
it must be labelled *media check*, never *signature verified*.

### 3. It must never be able to strand us

This registry already holds that a miss is harmless and that losing the
store costs rebuild time and nothing else (ADR-0002, ADR-0005).
Verification gets the same treatment, because a control that can leave
someone unable to install during an outage is one that gets worked
around — and then there is neither safety nor an honest record of its
absence.

- the public key is pinned locally, never fetched at verify time; no
  expiry and no revocation lookup, so an ISO from two years ago still
  verifies on a laptop with no route to anything;
- **missing and invalid are different.** No signature is absence of
  evidence: refuse by default, documented per-invocation override,
  loud. A signature that does not match is affirmative evidence the
  bytes changed: refuse harder, a distinct override, never the same
  flag. Most bad days land in the first bucket and stay recoverable;
- the override is a flag and not a setting, because a toggle that can
  be left on will be;
- nothing in the boot path — verifying in bootloader or initramfs turns
  a failure into an unbootable stick with no recourse;
- the default path verifies, so nobody has to remember: a documented
  `curl` plus a `sha256sum` someone is meant to run gets skipped the
  third time and every time after.

### 4. The signature is a sibling object

`<name>-<version>-<release>-<arch>.iso.minisig`, sharing its artifact's
stem so release and architecture parse identically on both and
canonicalising either produces the other's counterpart.

A sibling and not metadata because **this store has no metadata**:
every name is a symlink to a blob, there is no index, and nothing can
drift out of step because nothing holds pairs together. A signature
that is just another blob with a name inherits all of that. The
verifier is also a standalone tool on a laptop that has just downloaded
a file, so "fetch the file next to it" beats parsing JSON, and the
offline requirement makes a metadata round-trip actively unhelpful.

**Deletes are not coupled.** No refusing to delete an ISO while its
signature exists, and no deleting both together: that would put the
first relationship *between* entries into a store whose strength is
that entries have none. Neither drift direction is dangerous — an
orphaned signature is inert, and a missing one is exactly the *missing*
path above. If the tidiness matters, `gc` reporting orphaned signatures
costs no invariant.

**Amended by #21 (2026-09-21): the orphan is not inert.** Measured
against a throwaway instance: delete a signed ISO, and its signature is
still published; push *different* bytes under that same name, and the
store answers `201`. Section 5's gate looks a signature up by the name
it derives from the artifact, so the leftover satisfies it, and the
store ends up serving a bootable whose own published signature fails
`minisign -Vm` against it. The gate that exists to refuse unsigned
bootables was defeated by its own leftovers.

"An orphaned signature is inert" was the load-bearing claim here, and
it was false. So the two now leave together, in `store_unpublish()`.

The objection above still deserves an answer, because it was the right
question: does this put the first relationship *between* entries into a
store whose strength is that entries have none? It does not, because
that relationship already existed — section 5's gate derives one name
from the other and looks it up, which is the same rule read in the
other direction. Nothing is stored that holds a pair together; there is
still no index, and a signature is still just another blob with a name.
What changed is that a rule the accept path already applied is now
applied by the delete path too, so the two cannot disagree.

Note what is NOT coupled, deliberately: deleting a signature by its own
name removes only the signature, and an artifact may still be left
unsigned. Only the dangerous direction is closed. The `gc` reporting
suggested above is no longer needed for the case that motivated it,
though it would still find orphans left by an older daemon.

### 5. An unsigned bootable is refused, at header time

The cache is not vouching for anything by refusing; it is declining to
store an unsigned bootable, which is its own business.

Checked before any of the body is staged, so refusing costs a header
exchange rather than a multi-gigabyte upload. The signature must exist
first — it is made over the ISO's digest and can be produced before
either is uploaded, so that is the only order satisfying the rule.

This is a rule about **accepting**, not about maintaining. The store
declines to take an unsigned bootable; it does not promise one stays
signed forever, and it is a fine enough line to state rather than
infer. (As first written this sentence continued "that is what keeps it
consistent with deletes being uncoupled" — see the amendment in section
4, which is what that consistency turned out to cost.)

Note it blocks *publishing* and never *recovering*: if signing breaks,
no new ISO can be pushed, but every ISO already stored still fetches
and verifies and the known-good stick still boots. Publishing is the
right thing to make strict, because failing it costs a delay rather
than a machine.

### 6. One artifact, one row

A signature is a column and not a row: `signed` on the ISO's own entry
in `/api/v1/artifacts`, a column in the dashboard and `cixcachectl ls`
that appears only once something in view carries one, and a nested
`signature` object in `MANIFEST.json`. (Since #22 that object sits
inside the `formats` element it signs rather than directly on the
entry, because each encoding is signed separately — and for the same
reason a *package's* signature now appears there too, which the flat
shape had no room for.) The
artifact count keeps counting artifacts a person would recognise as
one; the signature's bytes still show in the size, because that is disk
truth.

The filtering is in the listing and **never in `store_walk()`**. That
was proposed and withdrawn during design: `store_gc()` builds its live
set from that walk, so a walk skipping signatures would leave their
blobs unreferenced and the next collection would delete every one of
them — the same class of bug as #6. The walk reports the store as it
is; presentation decides what to show. A test asserts a real collection
leaves a signature's blob alone.

### 7. An installer is counted apart from a package

`installers` and `installer_bytes` in `/api/v1/status`, a figure in the
menu bar, and a line in `cixcachectl status` — each appearing only when
there is at least one, on the same rule as everything else here: a
figure that always reads zero is noise.

Apart, and not folded in, because an ISO is not a package. Nothing
resolves it by `name@version`, no recipe stands behind it, and it is
booted rather than installed. Counting it inside "N packages" would
make that number mean two kinds of thing at once, and the first person
to reconcile it against `MANIFEST.json` — which has always had them in
separate sections — would find the two disagreeing.

A signature's bytes count toward its installer rather than toward
packages, since that is what they belong to.

One consequence worth stating because it looks like a contradiction:
`cixcachectl ls` lists packages and installers together, so its total
counts both, while `status` reports them separately. When both are
present the listing breaks its total down rather than leaving two
numbers that appear to disagree.

### 8. Installers are their own manifest section

`installers`, not inside `packages`. `packages` is where a package is
looked up by `name@version`; an ISO is never installed that way and has
no recipe behind it. Mixed in, every consumer of `packages` would grow
a filter, and the first one to forget it would try to install an ISO.

(As first written this said `packages` "is consumed by a daemon
resolving `name@version` for install". ADR-0002 is narrower and
governs: the daemon "never requests an index", and `MANIFEST.json` is a
convenience for people writing recipes. The argument above never needed
the daemon — that an ISO is not looked up by `name@version` is enough
on its own — so only the sentence changed.)

## Consequences

The guarantee is narrower than "the cache knows the right ISO", and
worth stating honestly: **the cache will not accept an unsigned
bootable, and cannot tell you whether a signature is good.** Only the
pinned key does that, on a machine that is not this one.

Nothing here is reachable until something signs an ISO — `cix#193`
item 1. The cache half is deliberately complete first so that the
signing work has somewhere to publish to.

The signature's sha256 in `MANIFEST.json` is for completeness, not for
checking: both it and the signature come from this server, so one
cannot vouch for the other. What approves a signature is a key that
does not come from here at all.
