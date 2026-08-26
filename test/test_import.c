/*
 * Migration of a hand-built static export into the content-addressed
 * store. The cases that matter: a plain file becomes a symlink into
 * blobs/, a re-run is a no-op, two identical files collapse onto one
 * blob, and a file whose bytes disagree with the shipped MANIFEST.json
 * is reported rather than quietly published.
 */
#include "importer.h"
#include "store.h"

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

static void write_file(const char *path, const char *content)
{
	FILE *f = fopen(path, "wb");

	if (f == NULL)
		return;
	fputs(content, f);
	fclose(f);
}

static int is_symlink(const char *path)
{
	struct stat st;

	return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}

static int count_blobs(const char *root)
{
	char cmd[512];
	char out[64];
	FILE *p;

	snprintf(cmd, sizeof(cmd), "ls -1 '%s/blobs' 2>/dev/null | wc -l", root);
	p = popen(cmd, "r");
	if (p == NULL)
		return -1;
	if (fgets(out, sizeof(out), p) == NULL)
		out[0] = '\0';
	pclose(p);
	return atoi(out);
}

int main(void)
{
	char root[] = "/tmp/cixcache-test-import-XXXXXX";
	struct import_stats st;
	char manifest[512];
	char path[512];
	char dup[512];
	char bad[512];

	if (mkdtemp(root) == NULL || store_init(root) != 0) {
		fprintf(stderr, "FAIL: cannot set up store\n");
		return 1;
	}
	snprintf(path, sizeof(path), "%s/packages/bash-5.2.37.tar.gz", root);
	snprintf(dup, sizeof(dup), "%s/packages/bash-5.2.37-2.tar.gz", root);
	snprintf(bad, sizeof(bad), "%s/packages/liar-1.0.tar.gz", root);
	write_file(path, "the same bytes");
	write_file(dup, "the same bytes");
	write_file(bad, "these bytes do not match the manifest");

	/*
	 * A manifest that records the truth for one file and a lie for
	 * another. The lie is the last moment the export's own record can
	 * still be checked against its bytes.
	 */
	snprintf(manifest, sizeof(manifest), "%s/MANIFEST.json", root);
	write_file(manifest,
	           "{\"images\":{},\"packages\":{"
	           "\"liar-1.0\":{\"file\":\"packages/liar-1.0.tar.gz\","
	           "\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\","
	           "\"bytes\":1}}}");

	memset(&st, 0, sizeof(st));
	CHECK(importer_run(root, manifest, 1, &st) != 0, "dry run reports the mismatch as a failure");
	CHECK(st.imported == 2, "dry run would import the two honest files");
	CHECK(st.mismatched == 1, "dry run catches the manifest disagreement");
	CHECK(!is_symlink(path), "dry run changed nothing on disk");
	CHECK(count_blobs(root) == 0, "dry run stored no blobs");

	memset(&st, 0, sizeof(st));
	importer_run(root, manifest, 0, &st);
	CHECK(st.imported == 2, "import moves the two honest files");
	CHECK(st.mismatched == 1, "import still refuses the mismatched one");
	CHECK(is_symlink(path), "an imported artifact is now a symlink");
	CHECK(is_symlink(dup), "the duplicate is a symlink too");
	CHECK(!is_symlink(bad), "the mismatched file was left alone");

	/* Identical bytes under two names cost exactly one blob. */
	CHECK(count_blobs(root) == 1, "duplicate content collapsed onto one blob");

	{
		char a[STORE_SHA256_MAX];
		char b[STORE_SHA256_MAX];

		CHECK(store_resolve(STORE_TIER_PACKAGE, "bash-5.2.37.tar.gz", a, sizeof(a)) == STORE_OK &&
		              store_resolve(STORE_TIER_PACKAGE, "bash-5.2.37-2.tar.gz", b, sizeof(b)) ==
		                      STORE_OK &&
		              strcmp(a, b) == 0,
		      "both names resolve to the same digest");
	}

	memset(&st, 0, sizeof(st));
	importer_run(root, manifest, 0, &st);
	CHECK(st.imported == 0, "a second run imports nothing");
	CHECK(st.already == 2, "a second run recognises what it already did");

	if (g_failures == 0)
		printf("test_import: ok\n");
	else
		printf("test_import: %d failure(s)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
