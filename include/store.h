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
	STORE_ERR_IO
};

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
 * Splits a published filename into a display name and version.
 *
 * DISPLAY ONLY. Nothing on the resolution path may call this, for the
 * reason given above. Guessing wrong in a table column is cosmetic;
 * guessing wrong while resolving a URL would not be.
 *
 * The version begins at the first hyphen followed by a digit, or by
 * 'v' and a digit. That is right for every artifact in this store
 * (libc-dev-2.36, nss-pam-ldapd-0.9.13-2, squashfs-tools-4.7.5-5,
 * openssh-10.4p1-8, cix-v2.1.1 included), and where it finds no such
 * boundary it puts everything in the name and leaves the version empty
 * rather than inventing one.
 */
void store_split_display(const char *name, char *out_name, size_t out_name_size, char *out_version,
                         size_t out_version_size);

/*
 * Resolves a published name to the digest its symlink points at,
 * without opening the blob. STORE_ERR_NOT_FOUND is the ordinary
 * answer for a miss and is never logged as an error -- a 404 here just
 * means "build it from source", and Cix hosts rely on that being cheap.
 */
enum store_error store_resolve(const char *name, char *out_digest, size_t out_digest_size);

/*
 * Resolves and opens. On STORE_OK the caller owns *out_fd. out_digest
 * may be NULL if the caller does not need it.
 */
enum store_error store_open(const char *name, int *out_fd, off_t *out_size, char *out_digest,
                            size_t out_digest_size);

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
 * Removes every blob no published name points at, and returns how many
 * went. Builds the live set by reading every symlink target, because
 * with symlinks a blob's inode carries no refcount.
 */
int store_gc(int dry_run, long long *out_bytes_freed);

const char *store_error_str(enum store_error e);

#endif /* STORE_H */
