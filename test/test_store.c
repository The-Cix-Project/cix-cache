/*
 * Store unit tests: name validation, publish/resolve, deduplication,
 * the immutability conflict, and garbage collection. Run directly --
 * build/test_store -- with no server involved.
 */
#include "store.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int g_failures;

#define CHECK(cond, msg)                                                                           \
	do {                                                                                           \
		if (!(cond)) {                                                                             \
			fprintf(stderr, "FAIL: %s\n", msg);                                                    \
			g_failures++;                                                                          \
		}                                                                                          \
	} while (0)

static const char *HEX64 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

static void write_file(const char *path, const char *content)
{
	FILE *f = fopen(path, "wb");

	if (f == NULL)
		return;
	fputs(content, f);
	fclose(f);
}

static void test_names(void)
{
	CHECK(store_name_is_valid("bash-5.2.37-2.tar.gz"), "plain package name");
	CHECK(store_name_is_valid("libc-dev-2.36.tar.gz"), "hyphenated package");
	CHECK(store_name_is_valid("nss-pam-ldapd-0.9.13-2.tar.gz"), "multi-hyphen package");

	CHECK(!store_name_is_valid("../etc/passwd.tar.gz"), "traversal rejected");
	CHECK(!store_name_is_valid("a/b.tar.gz"), "slash rejected");
	CHECK(!store_name_is_valid(".hidden.tar.gz"), "leading dot rejected");
	CHECK(!store_name_is_valid("bash-5.2.37.tar"), "wrong suffix rejected");
	CHECK(!store_name_is_valid("bash 5.tar.gz"), "space rejected");
	CHECK(!store_name_is_valid("bash%2e.tar.gz"), "percent rejected");

	CHECK(store_digest_is_valid(HEX64), "64 hex is a digest");
	CHECK(!store_digest_is_valid("abc"), "short string is not a digest");
	CHECK(!store_digest_is_valid("0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef"),
	      "uppercase is not a canonical digest");
}

/*
 * Display-only splitting. Every case here is a real name from the
 * store, chosen because each breaks a naive splitter in a different
 * way: a hyphen in the package name, two of them, a letter inside the
 * version, a "v" prefix, and a version with no digit boundary at all.
 */
static void test_split_display(void)
{
	static const struct {
		const char *file;
		const char *name;
		const char *version;
		int release;
	} cases[] = {
		{ "bash-5.2.37-2.tar.gz", "bash", "5.2.37", 2 },
		{ "libc-dev-2.36.tar.gz", "libc-dev", "2.36", 1 },
		{ "nss-pam-ldapd-0.9.13-2.tar.gz", "nss-pam-ldapd", "0.9.13", 2 },
		{ "squashfs-tools-4.7.5-5.tar.gz", "squashfs-tools", "4.7.5", 5 },
		{ "openssh-10.4p1-8.tar.gz", "openssh", "10.4p1", 8 },
		{ "openldap-client-2.6.14.tar.gz", "openldap-client", "2.6.14", 1 },
		{ "gcc-16.2.0-11.tar.gz", "gcc", "16.2.0", 11 },
		{ "cix-v2.1.1.tar.gz", "cix", "v2.1.1", 1 },
		{ "cix-v2.2.0-rc6-1.tar.gz", "cix", "v2.2.0-rc6", 1 },
		{ "tcc-0.9.27.tar.gz", "tcc", "0.9.27", 1 },
		/*
		 * A version that is itself all digits is still a version:
		 * there is no version before it for a release to belong to.
		 */
		{ "nano-8.tar.gz", "nano", "8", 1 },
		{ "noversion.tar.gz", "noversion", "", 1 }
	};
	char name[256];
	char version[256];
	char arch[64];
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		char msg[200];
		int release = -1;

		store_split_display(cases[i].file, name, sizeof(name), version, sizeof(version),
		                    &release, arch, sizeof(arch));
		snprintf(msg, sizeof(msg), "%s splits to '%s' + '%s' + r%d (got '%s' + '%s' + r%d)",
		         cases[i].file, cases[i].name, cases[i].version, cases[i].release, name, version,
		         release);
		CHECK(strcmp(name, cases[i].name) == 0 && strcmp(version, cases[i].version) == 0 &&
		              release == cases[i].release,
		      msg);
	}
}

/*
 * Canonical form is <name>-<version>-<release> with an omitted release
 * meaning 1. The cases that matter are the ones where a naive "last
 * component is the release" rule gets it wrong.
 */
static void test_canonical_name(void)
{
	static const struct {
		const char *in;
		const char *out;
		int changed;
	} cases[] = {
		{ "mtools-4.0.49.tar.gz", "mtools-4.0.49-1.tar.gz", 1 },
		{ "gawk-5.3.0-7.tar.gz", "gawk-5.3.0-7.tar.gz", 0 },
		{ "cix-v2.2.0-rc6-1.tar.gz", "cix-v2.2.0-rc6-1.tar.gz", 0 },
		{ "iputils-s20180629-1.tar.gz", "iputils-s20180629-1.tar.gz", 0 },
		/* Hyphens on both sides of the name/version boundary. */
		{ "openldap-client-2.6.14.tar.gz", "openldap-client-2.6.14-1.tar.gz", 1 },
		/* A date-style version must not be read as a huge release. */
		{ "foo-20250101.tar.gz", "foo-20250101-1.tar.gz", 1 },
		/* Nor a version that is a single integer. */
		{ "nano-8.tar.gz", "nano-8-1.tar.gz", 1 },
		/* One thing, one name: -007 and -7 cannot both be canonical. */
		{ "bash-5.2.37-007.tar.gz", "bash-5.2.37-7.tar.gz", 1 },
		/* No version means nothing for a release to qualify. */
		{ "noversion.tar.gz", "noversion.tar.gz", 0 }
	};
	char out[STORE_NAME_MAX];
	char again[STORE_NAME_MAX];
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		char msg[200];
		int rc = store_canonical_name(cases[i].in, out, sizeof(out));

		snprintf(msg, sizeof(msg), "%s canonicalizes to %s (got %s, rc %d)", cases[i].in,
		         cases[i].out, out, rc);
		CHECK(rc == cases[i].changed && strcmp(out, cases[i].out) == 0, msg);

		/* Canonicalizing a canonical name must be a no-op. */
		CHECK(store_canonical_name(out, again, sizeof(again)) == 0 && strcmp(again, out) == 0,
		      "canonical form is a fixed point");
	}
	CHECK(store_canonical_name("bad name.tar.gz", out, sizeof(out)) == -1,
	      "an invalid name has no canonical form");
	CHECK(store_canonical_name("notarball", out, sizeof(out)) == -1,
	      "a non-artifact name has no canonical form");
}

/*
 * The property the whole migration rests on: a non-canonical name is
 * an ALIAS of the canonical entry, not a second entry and not a miss.
 * One file exists; both spellings reach it.
 */
static void test_canonical_alias(const char *root)
{
	char digest[STORE_SHA256_MAX];
	char got[STORE_SHA256_MAX];
	char tmp[512];
	char path[512];
	struct stat st;

	snprintf(tmp, sizeof(tmp), "%s/tmp/alias", root);
	write_file(tmp, "alias payload");
	CHECK(store_hash_file(tmp, digest, sizeof(digest)) == 0, "hash the alias payload");
	CHECK(store_blob_adopt(tmp, digest) == STORE_OK, "adopt the alias blob");

	/* Published under the bare name... */
	CHECK(store_publish("aliaspkg-1.2.tar.gz", digest) == STORE_OK, "publish a bare name");

	/* ...lands on disk canonically, and only once. */
	snprintf(path, sizeof(path), "%s/%s/aliaspkg-1.2-1.tar.gz", root, STORE_DIR);
	CHECK(lstat(path, &st) == 0, "the bare push is stored canonically");
	snprintf(path, sizeof(path), "%s/%s/aliaspkg-1.2.tar.gz", root, STORE_DIR);
	CHECK(lstat(path, &st) != 0, "no second entry under the bare name");

	/* Both spellings resolve, to the same bytes. */
	CHECK(store_resolve("aliaspkg-1.2.tar.gz", got, sizeof(got)) == STORE_OK &&
	              strcmp(got, digest) == 0,
	      "the bare name resolves");
	CHECK(store_resolve("aliaspkg-1.2-1.tar.gz", got, sizeof(got)) == STORE_OK &&
	              strcmp(got, digest) == 0,
	      "the canonical name resolves to the same digest");

	/* Re-publishing the same bytes under the other spelling is idempotent,
	 * not a conflict -- they are one entry. */
	CHECK(store_publish("aliaspkg-1.2-1.tar.gz", digest) == STORE_OK,
	      "republishing the same bytes under the canonical name is idempotent");

	/* And unpublishing either spelling removes the one entry. */
	CHECK(store_unpublish("aliaspkg-1.2.tar.gz") == STORE_OK, "unpublish via the bare name");
	CHECK(store_resolve("aliaspkg-1.2-1.tar.gz", got, sizeof(got)) == STORE_ERR_NOT_FOUND,
	      "the canonical name is gone too -- it was one entry");
}

static void test_publish(const char *root)
{
	char digest_a[STORE_SHA256_MAX];
	char digest_b[STORE_SHA256_MAX];
	char got[STORE_SHA256_MAX];
	char tmp[512];
	char blob[512];
	off_t size = 0;
	int fd = -1;

	snprintf(tmp, sizeof(tmp), "%s/tmp/a", root);
	write_file(tmp, "artifact one");
	CHECK(store_hash_file(tmp, digest_a, sizeof(digest_a)) == 0, "hash a temp file");
	CHECK(store_blob_adopt(tmp, digest_a) == STORE_OK, "adopt a blob");
	CHECK(store_blob_exists(digest_a, &size), "adopted blob exists");
	CHECK(size == 12, "adopted blob has the right size");

	snprintf(blob, sizeof(blob), "%s/blobs/%s", root, digest_a);
	CHECK(access(blob, F_OK) == 0, "blob landed under blobs/");
	CHECK(access(tmp, F_OK) != 0, "temp file was moved, not copied");

	CHECK(store_publish("one-1.0.tar.gz", digest_a) == STORE_OK, "publish");
	CHECK(store_resolve("one-1.0.tar.gz", got, sizeof(got)) == STORE_OK,
	      "resolve a published name");
	CHECK(strcmp(got, digest_a) == 0, "resolve returns the digest it was published with");

	/* Republishing identical bytes is a no-op, not a conflict. */
	CHECK(store_publish("one-1.0.tar.gz", digest_a) == STORE_OK,
	      "idempotent republish");

	/* Two names, same bytes: one blob. That is the point of the store. */
	CHECK(store_publish("one-alias-1.0.tar.gz", digest_a) == STORE_OK,
	      "second name onto the same blob");

	snprintf(tmp, sizeof(tmp), "%s/tmp/b", root);
	write_file(tmp, "artifact two");
	CHECK(store_hash_file(tmp, digest_b, sizeof(digest_b)) == 0, "hash a second file");
	CHECK(strcmp(digest_a, digest_b) != 0, "different content, different digest");
	CHECK(store_blob_adopt(tmp, digest_b) == STORE_OK, "adopt the second blob");

	/*
	 * A recipe version is immutable in git, so a published name may
	 * only ever mean one byte sequence.
	 */
	CHECK(store_publish("one-1.0.tar.gz", digest_b) == STORE_ERR_CONFLICT,
	      "republish with different bytes is a conflict");
	CHECK(store_resolve("one-1.0.tar.gz", got, sizeof(got)) == STORE_OK &&
	              strcmp(got, digest_a) == 0,
	      "a rejected republish did not disturb the existing name");

	CHECK(store_open("one-1.0.tar.gz", &fd, &size, got, sizeof(got), NULL, 0) ==
	              STORE_OK,
	      "open a published artifact");
	if (fd >= 0)
		close(fd);
	CHECK(store_resolve("absent-9.9.tar.gz", got, sizeof(got)) ==
	              STORE_ERR_NOT_FOUND,
	      "a miss is NOT_FOUND, not an error");
}

static void test_gc(const char *root)
{
	char digest[STORE_SHA256_MAX];
	long long freed = 0;
	char tmp[512];
	int removed;

	snprintf(tmp, sizeof(tmp), "%s/tmp/orphan", root);
	write_file(tmp, "nobody points at me");
	store_hash_file(tmp, digest, sizeof(digest));
	store_blob_adopt(tmp, digest);

	/*
	 * Two orphans are expected, not one: the blob whose publish was
	 * refused as a conflict above is unreferenced too. A rejected
	 * push leaving its bytes behind for the collector is the intended
	 * behaviour -- the alternative is deleting a blob some other name
	 * may legitimately share.
	 */
	removed = store_gc(1, &freed);
	CHECK(removed == 2, "dry-run gc finds the orphan and the rejected push's blob");
	CHECK(freed > 0, "dry-run gc reports the bytes it would free");
	CHECK(store_blob_exists(digest, NULL), "dry-run gc removed nothing");

	removed = store_gc(0, &freed);
	CHECK(removed == 2, "gc removes both orphans");
	CHECK(!store_blob_exists(digest, NULL), "orphan blob is gone");
	CHECK(store_resolve("one-1.0.tar.gz", digest, sizeof(digest)) == STORE_OK,
	      "gc left published blobs alone");
}

/*
 * A store written before canonical names existed, migrated.
 *
 * The load-bearing assertion is the gc one. store_walk() builds the
 * collector's live set, and if it resolved names through the
 * canonicalising path it would silently skip every entry not yet
 * migrated -- which would make gc free the blobs of all of them. This
 * caught exactly that during development.
 */
static void test_canonicalize_migration(const char *root)
{
	char digest[STORE_SHA256_MAX];
	char got[STORE_SHA256_MAX];
	char tmp[512];
	char link[512];
	char target[128];
	long long freed = 0;
	int renamed = 0;
	int conflicts = 0;
	struct stat st;

	snprintf(tmp, sizeof(tmp), "%s/tmp/legacy", root);
	write_file(tmp, "legacy export bytes");
	CHECK(store_hash_file(tmp, digest, sizeof(digest)) == 0, "hash the legacy payload");
	CHECK(store_blob_adopt(tmp, digest) == STORE_OK, "adopt the legacy blob");

	/* Placed by hand, exactly as an old export left it: bare name. */
	snprintf(link, sizeof(link), "%s/%s/legacy-1.0.tar.gz", root, STORE_DIR);
	snprintf(target, sizeof(target), "../blobs/%s", digest);
	CHECK(symlink(target, link) == 0, "place a pre-migration entry by hand");

	CHECK(store_gc(0, &freed) >= 0, "collect over a store holding an unmigrated entry");
	CHECK(store_blob_exists(digest, NULL), "an unmigrated entry still protects its blob");

	CHECK(store_canonicalize(0, &renamed, &conflicts) == 0, "migrate the store");
	CHECK(renamed == 1 && conflicts == 0, "exactly the one bare entry was renamed");

	snprintf(link, sizeof(link), "%s/%s/legacy-1.0-1.tar.gz", root, STORE_DIR);
	CHECK(lstat(link, &st) == 0, "the entry now exists under its canonical name");
	snprintf(link, sizeof(link), "%s/%s/legacy-1.0.tar.gz", root, STORE_DIR);
	CHECK(lstat(link, &st) != 0, "and no longer under the bare one");

	/* The whole point of the migration being safe: old URLs still work. */
	CHECK(store_resolve("legacy-1.0.tar.gz", got, sizeof(got)) == STORE_OK &&
	              strcmp(got, digest) == 0,
	      "the pre-migration URL still resolves");

	/* Idempotent: a second pass has nothing left to do. */
	CHECK(store_canonicalize(0, &renamed, &conflicts) == 0 && renamed == 0,
	      "migrating an already-canonical store is a no-op");
}

/*
 * Listings have to be ordered. readdir order is whatever the
 * filesystem's hashing produced, which for a few hundred rows is just
 * noise -- and worse, it is not stable, so a manifest generated twice
 * from the same store can differ.
 */
static void test_list_order(const char *root)
{
	struct store_entry *ents = NULL;
	char digest[STORE_SHA256_MAX];
	char tmp[512];
	char link[512];
	char target[128];
	int n;
	int i;
	int ok;

	snprintf(tmp, sizeof(tmp), "%s/tmp/ordered", root);
	write_file(tmp, "ordered payload");
	CHECK(store_hash_file(tmp, digest, sizeof(digest)) == 0, "hash the ordering payload");
	CHECK(store_blob_adopt(tmp, digest) == STORE_OK, "adopt the ordering blob");

	/* Three names, published with mtimes a minute apart. */
	snprintf(target, sizeof(target), "../blobs/%s", digest);
	for (i = 0; i < 3; i++) {
		struct timespec times[2];

		snprintf(link, sizeof(link), "%s/%s/ord%d-1.0-1.tar.gz", root, STORE_DIR, i);
		CHECK(symlink(target, link) == 0, "place an ordering entry");
		times[0].tv_sec = 1000000 + i * 60;
		times[0].tv_nsec = 0;
		times[1] = times[0];
		CHECK(utimensat(AT_FDCWD, link, times, AT_SYMLINK_NOFOLLOW) == 0, "set its mtime");
	}

	n = store_list(&ents);
	CHECK(n >= 3, "store_list returns every entry");

	qsort(ents, (size_t)n, sizeof(*ents), store_cmp_newest);
	ok = 1;
	for (i = 1; i < n; i++) {
		if (ents[i - 1].mtime < ents[i].mtime)
			ok = 0;
	}
	CHECK(ok, "newest first: mtime never increases down the list");

	/*
	 * Relative order among the three placed here, not absolute
	 * position: the store also holds entries from the tests above,
	 * stamped with the real clock.
	 */
	{
		int at[3];

		for (i = 0; i < 3; i++)
			at[i] = -1;
		for (i = 0; i < n; i++) {
			if (strncmp(ents[i].name, "ord", 3) == 0 && ents[i].name[3] >= '0' &&
			    ents[i].name[3] <= '2')
				at[ents[i].name[3] - '0'] = i;
		}
		CHECK(at[0] >= 0 && at[1] >= 0 && at[2] >= 0, "all three ordering entries are listed");
		CHECK(at[2] < at[1] && at[1] < at[0], "the most recently published of them comes first");
	}

	/*
	 * The three placed here share nothing but their mtimes are
	 * distinct; give two the same mtime and the tiebreak must still
	 * produce one definite order, or rows swap between polls.
	 */
	qsort(ents, (size_t)n, sizeof(*ents), store_cmp_name);
	ok = 1;
	for (i = 1; i < n; i++) {
		if (strcmp(ents[i - 1].name, ents[i].name) >= 0)
			ok = 0;
	}
	CHECK(ok, "by name: strictly ascending, so a manifest is reproducible");

	free(ents);
}

/*
 * Architecture in a name (ADR-0008).
 *
 * The assertion that matters is the ambiguous one. A checksum cannot
 * tell an aarch64 binary from an x86_64 one -- the bytes are exactly
 * the bytes that were published -- so serving the wrong machine's
 * artifact is the one failure the content-addressed design cannot
 * catch. When a bare name could mean either, it has to mean neither.
 */
static void test_arch(const char *root)
{
	char digest_x[STORE_SHA256_MAX];
	char digest_a[STORE_SHA256_MAX];
	char digest_bare[STORE_SHA256_MAX];
	char got[STORE_SHA256_MAX];
	char resolved[STORE_NAME_MAX];
	char out[STORE_NAME_MAX];
	char tmp[512];
	int renamed = 0;
	int conflicts = 0;

	/* Canonical form carries an architecture through, and never adds one. */
	CHECK(store_canonical_name("tcc-0.9.27-7-x86_64.tar.gz", out, sizeof(out)) == 0 &&
	              strcmp(out, "tcc-0.9.27-7-x86_64.tar.gz") == 0,
	      "an arch-qualified canonical name is unchanged");
	CHECK(store_canonical_name("tcc-0.9.27-x86_64.tar.gz", out, sizeof(out)) == 1 &&
	              strcmp(out, "tcc-0.9.27-1-x86_64.tar.gz") == 0,
	      "the release is added behind the architecture, not in front of it");
	CHECK(store_canonical_name("tcc-0.9.27-7.tar.gz", out, sizeof(out)) == 0 &&
	              strcmp(out, "tcc-0.9.27-7.tar.gz") == 0,
	      "a name with no architecture does not acquire one");
	CHECK(store_arch_of("tcc-0.9.27-7-aarch64.tar.gz", out, sizeof(out)) != NULL &&
	              strcmp(out, "aarch64") == 0,
	      "the architecture is read back off a name");
	CHECK(store_arch_of("tcc-0.9.27-7.tar.gz", NULL, 0) == NULL, "and is absent when absent");
	/* Only a listed word counts, or ordinary text becomes an architecture. */
	CHECK(store_arch_of("foo-1.0-1-sparc64.tar.gz", NULL, 0) == NULL,
	      "an unlisted trailing word is not an architecture");

	snprintf(tmp, sizeof(tmp), "%s/tmp/archx", root);
	write_file(tmp, "x86_64 bytes");
	CHECK(store_hash_file(tmp, digest_x, sizeof(digest_x)) == 0, "hash the x86_64 payload");
	CHECK(store_blob_adopt(tmp, digest_x) == STORE_OK, "adopt it");
	CHECK(store_publish("archpkg-1.0-1-x86_64.tar.gz", digest_x) == STORE_OK,
	      "publish for one architecture");

	/* One architecture: a bare name is unambiguous, so old recipes work. */
	CHECK(store_resolve_as("archpkg-1.0-1.tar.gz", got, sizeof(got), resolved,
	                       sizeof(resolved)) == STORE_OK &&
	              strcmp(got, digest_x) == 0,
	      "a bare name resolves while one architecture exists");
	CHECK(strcmp(resolved, "archpkg-1.0-1-x86_64.tar.gz") == 0,
	      "and reports which entry answered, so the alias can be logged");
	/* The release-less spelling too -- both aliases compose. */
	CHECK(store_resolve("archpkg-1.0.tar.gz", got, sizeof(got)) == STORE_OK,
	      "the pre-release-suffix spelling still resolves as well");

	snprintf(tmp, sizeof(tmp), "%s/tmp/archa", root);
	write_file(tmp, "aarch64 bytes");
	CHECK(store_hash_file(tmp, digest_a, sizeof(digest_a)) == 0, "hash the aarch64 payload");
	CHECK(store_blob_adopt(tmp, digest_a) == STORE_OK, "adopt it");
	CHECK(store_publish("archpkg-1.0-1-aarch64.tar.gz", digest_a) == STORE_OK,
	      "the second architecture is a separate entry, not a conflict");

	/* Two architectures: the bare name must now refuse, not pick. */
	CHECK(store_resolve("archpkg-1.0-1.tar.gz", got, sizeof(got)) == STORE_ERR_AMBIGUOUS,
	      "a bare name stops resolving once two architectures have it");
	CHECK(store_resolve("archpkg-1.0.tar.gz", got, sizeof(got)) == STORE_ERR_AMBIGUOUS,
	      "and so does the release-less spelling of it");

	/* Naming one still works, and gets that one's bytes. */
	CHECK(store_resolve("archpkg-1.0-1-x86_64.tar.gz", got, sizeof(got)) == STORE_OK &&
	              strcmp(got, digest_x) == 0,
	      "asking for x86_64 gets the x86_64 bytes");
	CHECK(store_resolve("archpkg-1.0-1-aarch64.tar.gz", got, sizeof(got)) == STORE_OK &&
	              strcmp(got, digest_a) == 0,
	      "asking for aarch64 gets the aarch64 bytes");
	CHECK(store_resolve("archpkg-1.0-1-riscv64.tar.gz", got, sizeof(got)) == STORE_ERR_NOT_FOUND,
	      "asking for an architecture nobody published is an ordinary miss");

	/*
	 * #6. An entry stored WITHOUT an architecture is a candidate, not
	 * an answer. Resolution tries the exact name first, so returning
	 * it the moment it matched let it win before the architectures
	 * were looked at -- and an unstamped artifact then bypassed the
	 * ambiguity check entirely. Every push from a daemon that sends no
	 * architecture creates one of these, so ordinary use widened the
	 * hole.
	 */
	snprintf(tmp, sizeof(tmp), "%s/tmp/archbare", root);
	write_file(tmp, "unstamped, really x86_64");
	CHECK(store_hash_file(tmp, digest_bare, sizeof(digest_bare)) == 0, "hash the unstamped payload");
	CHECK(store_blob_adopt(tmp, digest_bare) == STORE_OK, "adopt it");
	CHECK(store_publish("barepkg-2.0-1.tar.gz", digest_bare) == STORE_OK,
	      "publish with no architecture at all");

	/* Alone, it is the only candidate, so it still resolves. */
	CHECK(store_resolve("barepkg-2.0-1.tar.gz", got, sizeof(got)) == STORE_OK &&
	              strcmp(got, digest_bare) == 0,
	      "an unstamped artifact resolves while nothing competes with it");

	CHECK(store_publish("barepkg-2.0-1-aarch64.tar.gz", digest_a) == STORE_OK,
	      "now publish another machine's build under the same name");
	CHECK(store_resolve("barepkg-2.0-1.tar.gz", got, sizeof(got)) == STORE_ERR_AMBIGUOUS,
	      "the unstamped entry no longer wins by being asked for exactly");
	/* Naming one still works, and the unstamped one is still reachable. */
	CHECK(store_resolve("barepkg-2.0-1-aarch64.tar.gz", got, sizeof(got)) == STORE_OK &&
	              strcmp(got, digest_a) == 0,
	      "naming the architecture still resolves");

	CHECK(store_unpublish("barepkg-2.0-1.tar.gz") == STORE_OK, "clean up the unstamped entry");
	CHECK(store_resolve("barepkg-2.0-1.tar.gz", got, sizeof(got)) == STORE_OK &&
	              strcmp(got, digest_a) == 0,
	      "with it gone the bare name is unambiguous again");
	CHECK(store_unpublish("barepkg-2.0-1-aarch64.tar.gz") == STORE_OK, "clean up");

	/*
	 * The same artifact under two spellings is not ambiguous. Stamping
	 * a store after something was pushed bare, and the bare name being
	 * pushed again afterwards, leaves exactly this -- and refusing it
	 * would 409 a name whose answer is not in doubt. Seen live on a
	 * zlib that had been published both ways.
	 */
	CHECK(store_publish("twin-1.0-1-x86_64.tar.gz", digest_x) == STORE_OK, "publish stamped");
	{
		char link[512];
		char target[128];

		snprintf(link, sizeof(link), "%s/%s/twin-1.0-1.tar.gz", root, STORE_DIR);
		snprintf(target, sizeof(target), "../blobs/%s", digest_x);
		CHECK(symlink(target, link) == 0, "and the same bytes under the bare name");
	}
	CHECK(store_resolve("twin-1.0-1.tar.gz", got, sizeof(got)) == STORE_OK &&
	              strcmp(got, digest_x) == 0,
	      "two spellings of identical bytes resolve, they are not a choice");
	/* But a genuine disagreement still refuses. */
	CHECK(store_publish("twin-1.0-1-aarch64.tar.gz", digest_a) == STORE_OK,
	      "now a different machine's build joins them");
	CHECK(store_resolve("twin-1.0-1.tar.gz", got, sizeof(got)) == STORE_ERR_AMBIGUOUS,
	      "and the bare name refuses again, because now the bytes differ");
	CHECK(store_unpublish("twin-1.0-1-x86_64.tar.gz") == STORE_OK, "clean up");
	CHECK(store_unpublish("twin-1.0-1-aarch64.tar.gz") == STORE_OK, "clean up");
	CHECK(store_unpublish("twin-1.0-1.tar.gz") == STORE_OK, "clean up");

	/* The stamp refuses a machine it does not know, rather than inventing it. */
	CHECK(store_set_arch("pdp11", 1, &renamed, &conflicts) == -1,
	      "an unknown architecture is refused");

	CHECK(store_unpublish("archpkg-1.0-1-x86_64.tar.gz") == STORE_OK, "clean up x86_64");
	CHECK(store_unpublish("archpkg-1.0-1-aarch64.tar.gz") == STORE_OK, "clean up aarch64");
}

/*
 * Version ordering (#5). The cases are the ones this store actually
 * holds, plus the two that plain string collation gets backwards.
 */
static void test_version_cmp(void)
{
	static const struct {
		const char *lo;
		const char *hi;
		const char *why;
	} pairs[] = {
		/* The bug this was filed for: a candidate precedes its release. */
		{ "v2.2.0-rc6", "v2.2.0", "a prerelease is older than its release" },
		{ "v2.2.0-rc6", "v2.2.0-rc10", "rc6 is older than rc10, not newer" },
		/* Numeric, where collation alone gets it wrong. */
		{ "2.1.8", "2.1.10", "2.1.10 follows 2.1.8" },
		{ "1.2", "1.2.1", "a shorter version is older" },
		{ "1.9", "1.10", "ten follows nine" },
		/*
		 * Patch levels are NOT prereleases: no hyphen, so they are
		 * newer than the version they patch. Both are in this store.
		 */
		{ "10.4", "10.4p1", "openssh's p1 is a patch level, so newer" },
		{ "1.5.8", "1.5.8.pl02", "xorriso's pl02 likewise" },
		/* Text vs number in the same position. */
		{ "1.beta", "1.2", "a number outranks text" },
		{ "s20180629", "s20180630", "iputils' date-ish versions still order" }
	};
	size_t i;

	for (i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
		char msg[220];

		snprintf(msg, sizeof(msg), "%s < %s -- %s", pairs[i].lo, pairs[i].hi, pairs[i].why);
		CHECK(store_version_cmp(pairs[i].lo, pairs[i].hi) < 0, msg);
		snprintf(msg, sizeof(msg), "%s > %s (the other way round)", pairs[i].hi, pairs[i].lo);
		CHECK(store_version_cmp(pairs[i].hi, pairs[i].lo) > 0, msg);
	}

	/* Equality, including the spellings that mean the same thing. */
	CHECK(store_version_cmp("2.1.1", "2.1.1") == 0, "a version equals itself");
	CHECK(store_version_cmp("v2.1.1", "2.1.1") == 0, "a leading v is not part of the version");
	CHECK(store_version_cmp("1.007", "1.7") == 0, "leading zeros are not magnitude");
	CHECK(store_version_cmp("", "") == 0, "two absent versions are equal");
	CHECK(store_version_cmp("", "1.0") < 0, "an absent version is older than any version");
}

/*
 * Ordering revisions of ONE upstream version -- the commonest shape in
 * this store, and the one the comparator originally got backwards. All
 * of these share a version, so ordering falls to the release, and a
 * string tiebreak puts 10 above 2 and 1 above 6.
 */
static void test_release_order(const char *root)
{
	static const char *const names[] = { "relpkg-1.3.2-2-x86_64.tar.gz",
	                                     "relpkg-1.3.2-10-x86_64.tar.gz",
	                                     "relpkg-1.3.2-1-x86_64.tar.gz",
	                                     "relpkg-1.3.2-12-x86_64.tar.gz",
	                                     "relpkg-1.3.2-9-x86_64.tar.gz" };
	static const int want[] = { 1, 2, 9, 10, 12 };
	struct store_entry *ents = NULL;
	struct store_entry *mine[5];
	char digest[STORE_SHA256_MAX];
	char tmp[512];
	int n;
	int i;
	int j;
	int k = 0;

	snprintf(tmp, sizeof(tmp), "%s/tmp/relpay", root);
	write_file(tmp, "release ordering payload");
	CHECK(store_hash_file(tmp, digest, sizeof(digest)) == 0, "hash the payload");
	CHECK(store_blob_adopt(tmp, digest) == STORE_OK, "adopt it");
	for (i = 0; i < 5; i++)
		CHECK(store_publish(names[i], digest) == STORE_OK, "publish a revision");

	n = store_list(&ents);
	CHECK(n > 0, "list the store");
	store_rank_versions(ents, n);

	for (i = 0; i < n && k < 5; i++) {
		char nm[STORE_NAME_MAX];
		char vv[STORE_NAME_MAX];

		store_split_display(ents[i].name, nm, sizeof(nm), vv, sizeof(vv), NULL, NULL, 0);
		if (strcmp(nm, "relpkg") == 0)
			mine[k++] = &ents[i];
	}
	CHECK(k == 5, "all five revisions listed");

	/* Selection sort by rank -- five items, and clearer than qsort here. */
	for (i = 0; i < k; i++) {
		for (j = i + 1; j < k; j++) {
			if (mine[j]->version_rank < mine[i]->version_rank) {
				struct store_entry *t = mine[i];

				mine[i] = mine[j];
				mine[j] = t;
			}
		}
	}
	for (i = 0; i < k; i++) {
		char nm[STORE_NAME_MAX];
		char vv[STORE_NAME_MAX];
		char msg[160];
		int rel = 0;

		store_split_display(mine[i]->name, nm, sizeof(nm), vv, sizeof(vv), &rel, NULL, 0);
		snprintf(msg, sizeof(msg), "position %d in version order is release %d, got %d", i,
		         want[i], rel);
		CHECK(rel == want[i], msg);
	}

	free(ents);
	for (i = 0; i < 5; i++)
		store_unpublish(names[i]);
}

/*
 * Bootables and their signatures (#8).
 *
 * The signature shares its artifact's stem, so release and
 * architecture parse identically on both and canonicalising either
 * produces the other's counterpart. That is what lets one be found
 * from the other with no index.
 */
static void test_signatures(void)
{
	char out[STORE_NAME_MAX];
	char name[STORE_NAME_MAX];
	char version[STORE_NAME_MAX];
	char arch[64];
	int release = -1;

	CHECK(store_suffix_of("a-1.0-1-x86_64.iso") != NULL &&
	              strcmp(store_suffix_of("a-1.0-1-x86_64.iso"), ".iso") == 0,
	      "an .iso is a recognised artifact");
	/* Longest-first matters: this must not be read as a bare suffix. */
	CHECK(store_suffix_of("a-1.0-1-x86_64.iso.minisig") != NULL &&
	              strcmp(store_suffix_of("a-1.0-1-x86_64.iso.minisig"), STORE_SIG_SUFFIX) == 0,
	      "a compound signature suffix wins over the shorter one it ends with");

	CHECK(store_is_signature("a-1.0-1-x86_64.iso.minisig"), "a .minisig is a signature");
	CHECK(!store_is_signature("a-1.0-1-x86_64.iso"), "an .iso is not");
	CHECK(!store_is_signature("a-1.0-1-x86_64.tar.gz"), "nor is a package");

	CHECK(store_needs_signature("a-1.0-1-x86_64.iso"), "a bootable must be signed");
	CHECK(!store_needs_signature("a-1.0-1-x86_64.tar.gz"),
	      "a package is approved by its recipe instead");
	CHECK(!store_needs_signature("a-1.0-1-x86_64.iso.minisig"),
	      "and a signature does not itself need one");

	CHECK(store_signature_name("a-1.0-1-x86_64.iso", out, sizeof(out)) == 0 &&
	              strcmp(out, "a-1.0-1-x86_64.iso.minisig") == 0,
	      "the signature name is derived from the artifact");
	CHECK(store_signature_name("a-1.0-1-x86_64.tar.gz", out, sizeof(out)) != 0,
	      "a package has no signature name");

	/* The stem parses the same either side of the suffix. */
	store_split_display("cix-installer-2.2.0-1-x86_64.iso", name, sizeof(name), version,
	                    sizeof(version), &release, arch, sizeof(arch));
	CHECK(strcmp(name, "cix-installer") == 0 && strcmp(version, "2.2.0") == 0 && release == 1 &&
	              strcmp(arch, "x86_64") == 0,
	      "an installer name splits like any other");
	store_split_display("cix-installer-2.2.0-1-x86_64.iso.minisig", name, sizeof(name), version,
	                    sizeof(version), &release, arch, sizeof(arch));
	CHECK(strcmp(name, "cix-installer") == 0 && strcmp(version, "2.2.0") == 0 && release == 1 &&
	              strcmp(arch, "x86_64") == 0,
	      "and so does its signature");

	/* Canonicalising either produces the other's counterpart. */
	CHECK(store_canonical_name("cix-installer-2.2.0-x86_64.iso", out, sizeof(out)) == 1 &&
	              strcmp(out, "cix-installer-2.2.0-1-x86_64.iso") == 0,
	      "an ISO gets its release behind the architecture");
	CHECK(store_canonical_name("cix-installer-2.2.0-x86_64.iso.minisig", out, sizeof(out)) == 1 &&
	              strcmp(out, "cix-installer-2.2.0-1-x86_64.iso.minisig") == 0,
	      "and so does its signature, to the matching name");
}

int main(void)
{
	char root[] = "/tmp/cixcache-test-store-XXXXXX";

	if (mkdtemp(root) == NULL) {
		fprintf(stderr, "FAIL: cannot create temp root\n");
		return 1;
	}
	if (store_init(root) != 0) {
		fprintf(stderr, "FAIL: store_init\n");
		return 1;
	}
	test_names();
	test_split_display();
	test_canonical_name();
	test_version_cmp();
	test_publish(root);
	test_gc(root);
	/*
	 * After the collector: this one adopts a blob and unpublishes it,
	 * which would otherwise show up as an extra orphan in gc's counts.
	 */
	test_canonical_alias(root);
	test_canonicalize_migration(root);
	test_release_order(root);
	test_list_order(root);
	test_arch(root);
	test_signatures();

	if (g_failures == 0)
		printf("test_store: ok\n");
	else
		printf("test_store: %d failure(s)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
