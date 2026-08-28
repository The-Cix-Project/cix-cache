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
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		char msg[200];
		int release = -1;

		store_split_display(cases[i].file, name, sizeof(name), version, sizeof(version),
		                    &release);
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

	CHECK(store_open("one-1.0.tar.gz", &fd, &size, got, sizeof(got)) ==
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
	test_publish(root);
	test_gc(root);
	/*
	 * After the collector: this one adopts a blob and unpublishes it,
	 * which would otherwise show up as an extra orphan in gc's counts.
	 */
	test_canonical_alias(root);
	test_canonicalize_migration(root);

	if (g_failures == 0)
		printf("test_store: ok\n");
	else
		printf("test_store: %d failure(s)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
