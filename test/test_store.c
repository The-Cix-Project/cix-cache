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
	char image[256];

	CHECK(store_name_is_valid(STORE_TIER_PACKAGE, "bash-5.2.37-2.tar.gz"), "plain package name");
	CHECK(store_name_is_valid(STORE_TIER_PACKAGE, "libc-dev-2.36.tar.gz"), "hyphenated package");
	CHECK(store_name_is_valid(STORE_TIER_PACKAGE, "nss-pam-ldapd-0.9.13-2.tar.gz"),
	      "multi-hyphen package");

	CHECK(!store_name_is_valid(STORE_TIER_PACKAGE, "../etc/passwd.tar.gz"), "traversal rejected");
	CHECK(!store_name_is_valid(STORE_TIER_PACKAGE, "a/b.tar.gz"), "slash rejected");
	CHECK(!store_name_is_valid(STORE_TIER_PACKAGE, ".hidden.tar.gz"), "leading dot rejected");
	CHECK(!store_name_is_valid(STORE_TIER_PACKAGE, "bash-5.2.37.tar"), "wrong suffix rejected");
	CHECK(!store_name_is_valid(STORE_TIER_PACKAGE, "bash 5.tar.gz"), "space rejected");
	CHECK(!store_name_is_valid(STORE_TIER_PACKAGE, "bash%2e.tar.gz"), "percent rejected");

	snprintf(image, sizeof(image), "dev-%s.tar.gz", HEX64);
	CHECK(store_name_is_valid(STORE_TIER_IMAGE, image), "image with 64 hex accepted");
	CHECK(!store_name_is_valid(STORE_TIER_IMAGE, "dev-1.0.0.tar.gz"),
	      "image without manifest hash rejected");
	CHECK(!store_name_is_valid(STORE_TIER_IMAGE, "dev-ZZZ.tar.gz"), "image with non-hex rejected");

	CHECK(store_digest_is_valid(HEX64), "64 hex is a digest");
	CHECK(!store_digest_is_valid("abc"), "short string is not a digest");
	CHECK(!store_digest_is_valid("0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef"),
	      "uppercase is not a canonical digest");
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

	CHECK(store_publish(STORE_TIER_PACKAGE, "one-1.0.tar.gz", digest_a) == STORE_OK, "publish");
	CHECK(store_resolve(STORE_TIER_PACKAGE, "one-1.0.tar.gz", got, sizeof(got)) == STORE_OK,
	      "resolve a published name");
	CHECK(strcmp(got, digest_a) == 0, "resolve returns the digest it was published with");

	/* Republishing identical bytes is a no-op, not a conflict. */
	CHECK(store_publish(STORE_TIER_PACKAGE, "one-1.0.tar.gz", digest_a) == STORE_OK,
	      "idempotent republish");

	/* Two names, same bytes: one blob. That is the point of the store. */
	CHECK(store_publish(STORE_TIER_PACKAGE, "one-alias-1.0.tar.gz", digest_a) == STORE_OK,
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
	CHECK(store_publish(STORE_TIER_PACKAGE, "one-1.0.tar.gz", digest_b) == STORE_ERR_CONFLICT,
	      "republish with different bytes is a conflict");
	CHECK(store_resolve(STORE_TIER_PACKAGE, "one-1.0.tar.gz", got, sizeof(got)) == STORE_OK &&
	              strcmp(got, digest_a) == 0,
	      "a rejected republish did not disturb the existing name");

	CHECK(store_open(STORE_TIER_PACKAGE, "one-1.0.tar.gz", &fd, &size, got, sizeof(got)) ==
	              STORE_OK,
	      "open a published artifact");
	if (fd >= 0)
		close(fd);
	CHECK(store_resolve(STORE_TIER_PACKAGE, "absent-9.9.tar.gz", got, sizeof(got)) ==
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
	CHECK(store_resolve(STORE_TIER_PACKAGE, "one-1.0.tar.gz", digest, sizeof(digest)) == STORE_OK,
	      "gc left published blobs alone");
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
	test_publish(root);
	test_gc(root);

	if (g_failures == 0)
		printf("test_store: ok\n");
	else
		printf("test_store: %d failure(s)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
