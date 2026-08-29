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
 *   <root>/packages/<name>-<version>.tar.gz  -> ../blobs/<sha256>
 *   <root>/tmp/                              upload staging, same fs
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
 * Charset gate for a published filename. Deliberately validates the
 * WHOLE basename rather than splitting it into name and version:
 * "<name>-<version>" cannot be split unambiguously (libc-dev-2.36 and
 * nss-pam-ldapd-0.9.13-2 both put hyphens on both sides), and a
 * splitter that guesses wrong is a parsing bug sitting directly on the
 * path-resolution hot path. The whole basename is the key, looked up
 * literally.
 *
 * Accepts [A-Za-z0-9._-] only, requires a .tar.gz suffix, rejects a
 * leading dot and any "..". Because this is a whitelist, traversal is
 * structurally impossible rather than filtered against.
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

/* Removes the published name only. Blobs are removed only by store_gc(). */
enum store_error store_unpublish(const char *name);

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
