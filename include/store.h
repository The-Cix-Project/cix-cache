#ifndef STORE_H
#define STORE_H

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

/*
 * The artifact store: content-addressed blobs, published under the
 * exact filenames a Cix host asks for.
 *
 *   <root>/blobs/<sha256>                    real bytes, mode 0444
 *   <root>/packages/<name>-<ver>-<rel>-<arch>.tar.gz  -> ../blobs/<sha256>
 *   <root>/packages/<name>-<ver>-<rel>-<arch>.cixpkg  -> ../blobs/<sha256>
 *   <root>/tmp/                              upload staging, same fs
 *
 * One flat directory whatever the suffix: an installer, a package in
 * either encoding and any of their signatures are all published names
 * side by side, and which tier a name belongs to is a property of its
 * suffix rather than of where it sits.
 *
 * The published entries are symlinks, and that is the whole point: the
 * link target IS the digest, so resolving a name to its checksum and
 * size is readlink() + stat() with no index file anywhere that could
 * drift from the tree. Deduplication falls out for free -- the several
 * near-identical package revisions in this store cost one copy of any
 * byte sequence they share.
 *
 * Hardlinks would give a cheaper garbage collector (st_nlink is the
 * refcount) but would not encode the digest, so every HEAD would have
 * to re-hash the file. Symlinks buy an O(1) hot path at the cost of an
 * O(n) sweep in a collector that runs approximately never. That is the
 * right way round.
 *
 * Packages are the only tier. A whole-image rootfs artifact is derived
 * from the packages it is composed of -- an image's version is
 * literally the hash of its sorted name@version manifest -- so storing
 * one duplicated bytes this store already held. See
 * docs/adr/0006-packages-are-the-only-tier.md.
 */

#define STORE_SHA256_HEX_LEN 64
#define STORE_SHA256_MAX 65 /* 64 hex + NUL, matching Cix's PKG_SHA256_MAX */
#define STORE_NAME_MAX 256
/*
 * The most store_canonical_name() can add to a name: the "-1" it
 * inserts when a name carries no release. Canonical form is produced
 * into a STORE_NAME_MAX buffer, so a name within this much of the
 * limit canonicalises to nothing at all rather than to something too
 * long -- see store_name_is_valid() for why the whitelist does not
 * reserve it.
 */
#define STORE_CANONICAL_GROWTH 2

/*
 * Absolute path, so a server started as PID 1 with an empty PATH still
 * finds it. That is not hypothetical: the Cix host's own export
 * endpoint is broken for exactly this reason (its tar -z shells out to
 * a bare gzip through PATH and the daemon has none, issue #125), which
 * is why every artifact in this store had to be extracted by hand.
 * Never hand-roll the hash either -- the sibling project forks this
 * same binary for every checksum it computes.
 */
#define STORE_SHA256SUM_BIN "/usr/bin/sha256sum"

/* Where published names live on disk, under the store root. */
#define STORE_DIR "packages"

enum store_error {
	STORE_OK = 0,
	STORE_ERR_INVALID_NAME,
	STORE_ERR_NOT_FOUND,
	STORE_ERR_CONFLICT,
	/*
	 * A name without an architecture matched more than one artifact.
	 * Refusing is the whole point: a checksum cannot catch an artifact
	 * that is intact but built for another machine, so the one thing
	 * this store must never do is pick one.
	 */
	STORE_ERR_AMBIGUOUS,
	STORE_ERR_IO
};

/*
 * Architectures this store recognises in a name, spelled as uname -m
 * spells them -- which is also how cix-build-system's ADR-0001 spells
 * them, and there must be exactly one spelling across the three repos.
 *
 * NULL-terminated. Recognition is a whitelist and not a pattern
 * because the trailing component of a name is otherwise ordinary text:
 * only a listed word is an architecture, so nothing else can
 * accidentally become one.
 */
const char *const *store_arches(void);

/*
 * The architecture encoded in a name, or NULL if it carries none.
 * out may be NULL when only the presence matters.
 */
const char *store_arch_of(const char *name, char *out, size_t out_size);

/*
 * Stamps an architecture onto every published entry that has none.
 *
 * This is an operator ASSERTION, not an inference, which is why it is
 * a separate command and takes the architecture as an argument.
 * Canonicalisation deliberately never invents one: storing a bare push
 * as x86_64 would attach a claim the pusher never made, and an aarch64
 * build pushed under a bare name would end up labelled x86_64 -- a
 * false statement about the bytes, which is worse than no statement.
 *
 * Renames symlinks only. Returns 0, or -1 on an error that stopped the
 * pass. Either count pointer may be NULL.
 */
int store_set_arch(const char *arch, int dry_run, int *out_renamed, int *out_conflicts);

/*
 * Creates root/{blobs,packages,tmp} if absent. Returns 0, or -1 with
 * the reason on stderr.
 */
int store_init(const char *root);
const char *store_root(void);

/*
 * The suffix a published name ends in, or NULL if it ends in none this
 * store recognises.
 *
 * One table, because the alternative is the seven-character `.tar.gz`
 * assumption spelled out at every site that needs a stem -- and adding
 * a second suffix by hand is how the fourteenth site gets missed. The
 * returned pointer is into that table, so it outlives the call.
 *
 * Matched longest-first, so a compound suffix wins over the shorter one
 * it ends with.
 */
const char *store_suffix_of(const char *name);

/*
 * A detached signature is a sibling object sharing its artifact's stem
 * -- cix-installer-2.2.0-1-x86_64.iso and .iso.minisig differ only in
 * suffix, so release and architecture parse identically on both, and
 * canonicalising either produces the other's counterpart.
 *
 * A sibling and not metadata because the store has no metadata: every
 * name is a symlink to a blob and there is no index to keep in step.
 * A signature that is just another blob with a name cannot drift out
 * of sync with anything, and the verifier -- which is a standalone
 * tool on a laptop that has just downloaded a file -- gets it with one
 * GET rather than by parsing JSON.
 */
/*
 * The tail a signature name ends in. A signature composes with ANY
 * artifact suffix -- zlib-1.3.2-11-x86_64.tar.gz.minisig signs a
 * package exactly as cix-installer-2.5.1-1-x86_64.iso.minisig signs an
 * installer -- so what identifies one is this tail, not any single
 * compound spelling of it.
 *
 * The compounds themselves stay explicit in the suffix table, and must.
 * Registering a bare ".minisig" would let it match
 * "...-x86_64.tar.gz.minisig" as the whole suffix, leaving ".tar.gz"
 * inside the stem -- and every stem consumer downstream then reads "gz"
 * as part of the release or the architecture. That does not fail, it
 * mis-stamps, which is worse.
 */
#define STORE_SIG_EXT ".minisig"

/* True for a signature object rather than something to install or boot. */
int store_is_signature(const char *name);

/*
 * True for an artifact that may not be published unsigned. Bootables
 * only: a package is approved by a checksum in its recipe, an ISO is
 * booted by a person with nothing else vouching for it.
 */
int store_needs_signature(const char *name);

/*
 * Which TIER a published name's bytes belong to, which is fixed and
 * never configurable. True for an installer AND for an installer's
 * signature, since a signature's bytes belong with what it signs --
 * so a caller listing artifacts rather than counting bytes must
 * exclude signatures itself, the way both MANIFEST.json sections do.
 *
 * Separate from store_needs_signature() since #15, and the separation
 * is load-bearing: this is what MANIFEST.json splits its "packages"
 * and "installers" sections on and what the status counters count,
 * while that one is policy an operator can change. While they were one
 * function, requiring a signature for a package suffix would have
 * moved every such package out of the section the Cix daemon installs
 * from -- a config key silently unpublishing the store's whole reason
 * to exist. See docs/adr/0012-cixpkg-and-signature-policy.md.
 */
int store_is_installer(const char *name);

/*
 * Sets which suffixes may not be published unsigned, from a comma
 * separated list ("" for none, ".iso" for the default). Entries are
 * matched with or without the leading dot, because an operator hand
 * edits this.
 *
 * Writes the require_sig field of the suffix table and nothing else,
 * so the table stays the only place this is recorded and no second
 * copy can disagree with it. The tier is not writable from here.
 *
 * Returns 0, or -1 if any entry named no suffix this store recognises,
 * with the first such entry copied to bad. Unrecognised entries are
 * skipped rather than fatal: refusing to start takes the cache down,
 * and a cache that is down means every host builds from source, which
 * is slow but safe. The caller is expected to say so loudly.
 */
int store_set_signature_policy(const char *list, char *bad, size_t bad_size);

/*
 * Builds the signature name for an artifact: <name>.minisig. Returns 0,
 * or -1 for a name that is not a valid artifact, or is itself a
 * signature.
 *
 * Deliberately NOT gated on store_needs_signature(). Whether an
 * artifact is REQUIRED to be signed and whether it CAN be is a
 * different question: only a bootable is refused for being unsigned,
 * but any artifact may carry a signature, and packages now do
 * (cix ADR-0279). Gating this on the requirement meant a package's
 * signature could not be named even once it existed.
 */
int store_signature_name(const char *name, char *out, size_t out_size);

/*
 * Charset gate for a published filename. Deliberately validates the
 * WHOLE basename rather than splitting it into name and version:
 * "<name>-<version>" cannot be split unambiguously (libc-dev-2.36 and
 * nss-pam-ldapd-0.9.13-2 both put hyphens on both sides), and a
 * splitter that guesses wrong is a parsing bug sitting directly on the
 * path-resolution hot path. The whole basename is the key, looked up
 * literally.
 *
 * Accepts [A-Za-z0-9._-] only, requires one of the suffixes the table
 * above recognises, rejects a leading dot and any "..". Because this is
 * a whitelist, traversal is structurally impossible rather than
 * filtered against.
 *
 * Returns 1 if valid, 0 otherwise.
 */
int store_name_is_valid(const char *name);

/* 1 if s is exactly 64 lowercase hex digits. */
int store_digest_is_valid(const char *s);

/*
 * Rewrites a published filename into canonical form.
 *
 * Canonical identity is <name>-<version>-<release>, and an omitted
 * release means 1: mtools-4.0.49.tar.gz is mtools-4.0.49-1.tar.gz.
 * Release 1 and not 0 because the repository has always behaved that
 * way -- every package that has been revised went bare -> -2, skipping
 * -1 entirely, which only makes sense if a bare version already meant
 * the first packaging. See docs/adr/0007-canonical-artifact-names.md.
 *
 * The rule is deliberately narrow: the release is the last
 * hyphen-separated component when it is all digits AND the component
 * before it contains a digit. The second half is what stops a
 * date-style version being eaten -- in foo-20250101 there is no
 * version for a release to be a release OF, so the digits are the
 * version. A release only exists relative to a version.
 *
 * Nothing before that tail is examined, because it does not need to
 * be: the name/version boundary cannot be found reliably anyway
 * (openldap-client-2.6.14 puts hyphens on both sides of it), and
 * canonical form only asks whether a release is already present.
 *
 * An explicit release is re-rendered without leading zeros, so -007
 * and -7 cannot both exist as names for one thing.
 *
 * Returns 1 if out differs from name, 0 if name was already canonical,
 * -1 if name is invalid or the result would not fit.
 */
int store_canonical_name(const char *name, char *out, size_t out_size);

/*
 * Renames every non-canonical published entry into canonical form.
 * Blobs are never touched -- this moves symlinks only, so it costs no
 * I/O proportional to the store's size and cannot lose bytes.
 *
 * A rename whose target already exists is refused and counted in
 * *out_conflicts rather than clobbering it: two names resolving to one
 * canonical name is a question for an operator, not something to
 * resolve by picking whichever came last out of readdir.
 *
 * Returns 0, or -1 on an error that stopped the pass. Either count
 * pointer may be NULL.
 */
int store_canonicalize(int dry_run, int *out_renamed, int *out_conflicts);

/*
 * Splits a published filename into a display name, version and release.
 *
 * DISPLAY ONLY. Nothing on the resolution path may call this, for the
 * reason given above. Guessing wrong in a table column is cosmetic;
 * guessing wrong while resolving a URL would not be.
 *
 * The release is taken by the rule described for
 * store_canonical_name(), and is reported as 1 when the name carries
 * none. The version then begins at the first hyphen followed by a
 * digit, or by 'v' and a digit. That is right for every artifact in
 * this store (libc-dev-2.36, nss-pam-ldapd-0.9.13-2,
 * squashfs-tools-4.7.5-5, openssh-10.4p1-8, cix-v2.1.1 included), and
 * where it finds no such boundary it puts everything in the name and
 * leaves the version empty rather than inventing one.
 *
 * out_release may be NULL.
 */
void store_split_display(const char *name, char *out_name, size_t out_name_size, char *out_version,
                         size_t out_version_size, int *out_release, char *out_arch,
                         size_t out_arch_size);

/*
 * Resolves a published name to the digest its symlink points at,
 * without opening the blob. STORE_ERR_NOT_FOUND is the ordinary
 * answer for a miss and is never logged as an error -- a 404 here just
 * means "build it from source", and Cix hosts rely on that being cheap.
 */
enum store_error store_resolve(const char *name, char *out_digest, size_t out_digest_size);

/*
 * Resolves, and reports which entry actually answered.
 *
 * A name carrying no architecture is matched against each known one,
 * and resolves only if EXACTLY ONE exists. That is the transitional
 * rule that lets recipes written before architectures keep working:
 * while the store holds one architecture a bare name is unambiguous,
 * and the moment a second appears the same name stops resolving rather
 * than starting to serve a coin flip. See
 * docs/adr/0008-architecture-in-artifact-names.md.
 *
 * out_name may be NULL; it receives the on-disk name that answered,
 * which is what lets a caller report that an alias was used.
 */
enum store_error store_resolve_as(const char *name, char *out_digest, size_t out_digest_size,
                                  char *out_name, size_t out_name_size);

/*
 * Resolves and opens. On STORE_OK the caller owns *out_fd. out_digest
 * may be NULL if the caller does not need it.
 */
enum store_error store_open(const char *name, int *out_fd, off_t *out_size, char *out_digest,
                            size_t out_digest_size, char *out_name, size_t out_name_size);

/*
 * Points name at digest. Idempotent when the name already resolves to
 * this same digest; STORE_ERR_CONFLICT when it resolves to a different
 * one.
 *
 * That conflict is a feature. A recipe version is immutable in git, so
 * a name may only ever mean one byte sequence; silently accepting a
 * republish would let an artifact drift out from under a recipe that
 * already built against it.
 */
enum store_error store_publish(const char *name, const char *digest);

/*
 * Removes the published name and the detached signature belonging to
 * it. Blobs are removed only by store_gc().
 *
 * The two leave together because an orphaned signature is not inert.
 * The publish gate looks one up by a name it derives exactly as
 * store_signature_name() does here, so a signature left behind by a
 * delete satisfies that gate for the NEXT push of the same name --
 * measured in #21, where re-publishing different bytes over a deleted
 * ISO returned 201 and left the store serving an artifact its own
 * published signature fails to verify. ADR-0010 section 4 is amended
 * accordingly; it had called the orphan inert.
 *
 * Removing a signature by its own name still removes only itself, so
 * the uncoupled delete the ADR wanted to keep is still available:
 * store_signature_name() refuses a name that is already a signature.
 *
 * *signature_removed, when non-NULL, reports whether one was found and
 * removed, so a caller can say so. STORE_ERR_NOT_FOUND still means the
 * ARTIFACT was absent -- a signature may have been removed on the way
 * to discovering that, which is how an existing orphan gets cleaned.
 */
enum store_error store_unpublish(const char *name, int *signature_removed);

/* 1 if blobs/<digest> exists; fills *out_size when non-NULL. */
int store_blob_exists(const char *digest, off_t *out_size);

/*
 * Moves an already-verified temp file into blobs/<digest>. If that blob
 * is already present the temp file is unlinked instead -- same bytes,
 * one copy. tmp/ is on the same filesystem as blobs/, so this is a
 * rename() and never a copy.
 */
enum store_error store_blob_adopt(const char *tmp_path, const char *digest);

/* Unique path under <root>/tmp for one upload in flight. */
int store_tmp_path(char *out, size_t out_size);

/*
 * Forks STORE_SHA256SUM_BIN and returns its 64 hex digits. Blocking --
 * the reactor must not call this on a large file, it forks the child
 * itself and watches a pidfd. Safe here for small files and for the
 * offline import path. out_size must be >= STORE_SHA256_MAX.
 */
int store_hash_file(const char *path, char *out, size_t out_size);

/*
 * Walks the store, calling fn for every published name in readdir
 * order. fn returns 0 to continue, non-zero to stop the walk (that
 * value is returned). Digest is the symlink target; size is the blob's
 * size, or -1 if the link dangles.
 *
 * mtime is the SYMLINK's own timestamp, not the blob's: it answers
 * "when did this name appear here", which is what an operator wants.
 * The blob's would answer "when were these bytes first seen under any
 * name", which for deduplicated content is some other artifact's
 * history.
 */
int store_walk(int (*fn)(const char *name, const char *digest, off_t size, time_t mtime, void *ctx),
               void *ctx);

/*
 * One published entry, as store_list() reports it.
 */
struct store_entry {
	char name[STORE_NAME_MAX];
	char digest[STORE_SHA256_MAX];
	off_t size;
	time_t mtime;
	/* Position in version order; see store_rank_versions(). */
	int version_rank;
};

/*
 * Collects every published entry into one array, which the caller owns
 * and must free(). Returns the count, or -1.
 *
 * Exists so listings can be ORDERED. store_walk() reports readdir
 * order, which is whatever the filesystem's hashing happened to
 * produce: fine for the collector, which only needs to see everything
 * once, and useless to a person reading a few hundred rows.
 */
int store_list(struct store_entry **out);

/*
 * Comparators for that array.
 *
 * store_cmp_newest -- most recently published first, ties broken on
 * name. The tiebreak is not cosmetic: mtimes are whole seconds and a
 * pushed batch ties constantly, and an order that is not total lets
 * rows swap places between the dashboard's polls.
 *
 * store_cmp_name -- by name. For MANIFEST.json, which exists to be
 * compared against another copy of itself. That file already leaves
 * out mtime so identical stores produce identical manifests; readdir
 * order defeated that on its own.
 */
int store_cmp_newest(const void *a, const void *b);
int store_cmp_name(const void *a, const void *b);

/*
 * Orders two upstream version strings. Returns -1, 0 or 1.
 *
 * Upstream versions are taken verbatim and are not semver, so this is
 * defined by rules that fit what is actually published rather than by
 * a standard nothing here follows:
 *
 *   1. A leading 'v' before a digit is ignored, so v2.1.1 and 2.1.1
 *      are the same version.
 *   2. The string splits at the first '-' into a version and a
 *      prerelease, as semver does. A version WITH a prerelease is
 *      older than the same version without one -- v2.2.0-rc6 comes
 *      before v2.2.0, which plain collation gets backwards because it
 *      is the longer string.
 *   3. Each side is then compared run by run, a run being consecutive
 *      digits or consecutive non-digits. Digit runs compare as
 *      numbers, so 2.1.10 follows 2.1.8. Text runs compare bytewise.
 *   4. Where one side has a digit run and the other has text, the
 *      digits are newer.
 *   5. Running out first is older. That is what makes 1.2 older than
 *      1.2.1 -- and, deliberately, what makes 10.4 older than 10.4p1
 *      and 1.5.8 older than 1.5.8.pl02, because those suffixes are
 *      patch levels rather than prereleases. Only a hyphen introduces
 *      a prerelease.
 *
 * These rules order every version in this store correctly. They cannot
 * order every string anyone might publish -- no rule can, when the
 * input is "whatever upstream called it" -- so the intent is to be
 * predictable and written down rather than clever.
 */
int store_version_cmp(const char *a, const char *b);

/*
 * Fills in version_rank for each entry: its index once the array is
 * ordered by store_version_cmp(), ties broken by name.
 *
 * A rank rather than the comparison itself, because the consumer is a
 * browser. Shipping a number the client sorts on keeps ONE
 * implementation of these rules -- a second one in JavaScript would be
 * a second thing to keep correct, and the two would drift.
 */
void store_rank_versions(struct store_entry *v, int n);

/*
 * Removes every blob no published name points at, and returns how many
 * went. Builds the live set by reading every symlink target, because
 * with symlinks a blob's inode carries no refcount.
 */
int store_gc(int dry_run, long long *out_bytes_freed);

const char *store_error_str(enum store_error e);

#endif /* STORE_H */
