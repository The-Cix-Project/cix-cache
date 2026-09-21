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
	char canonical[512];
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
	/*
	 * Under its CANONICAL name: the export was written before the
	 * naming standard existed, so importing one is also the moment it
	 * gets normalized. The bare path is gone -- adopt() moved those
	 * bytes into blobs/ -- and the entry that replaces it carries the
	 * release. Both spellings still resolve; only one file exists.
	 */
	snprintf(canonical, sizeof(canonical), "%s/packages/bash-5.2.37-1.tar.gz", root);
	CHECK(is_symlink(canonical), "an imported artifact is now a canonical symlink");
	CHECK(!is_symlink(path), "and is not left behind under its bare name");
	CHECK(is_symlink(dup), "the duplicate is a symlink too");
	CHECK(!is_symlink(bad), "the mismatched file was left alone");

	/* Identical bytes under two names cost exactly one blob. */
	CHECK(count_blobs(root) == 1, "duplicate content collapsed onto one blob");

	{
		char a[STORE_SHA256_MAX];
		char b[STORE_SHA256_MAX];

		CHECK(store_resolve("bash-5.2.37.tar.gz", a, sizeof(a)) == STORE_OK &&
		              store_resolve("bash-5.2.37-2.tar.gz", b, sizeof(b)) ==
		                      STORE_OK &&
		              strcmp(a, b) == 0,
		      "both names resolve to the same digest");
	}

	memset(&st, 0, sizeof(st));
	importer_run(root, manifest, 0, &st);
	CHECK(st.imported == 0, "a second run imports nothing");
	CHECK(st.already == 2, "a second run recognises what it already did");

	/*
	 * A manifest in the SERVED shape imports too.
	 *
	 * The importer's own input is an export's checked-in record
	 * (ADR-0001), which is flat: one { file, sha256, bytes } per
	 * identity. A manifest this server generates is not -- since #22
	 * an identity carries a "formats" array, because it may hold both
	 * a .cixpkg and a .tar.gz. Nothing stops somebody saving a served
	 * manifest beside a tree and importing that. Before the importer
	 * read both shapes the result was not a loud failure but a silent
	 * one: no entry matched, so every artifact looked unmentioned, and
	 * an unmentioned artifact is imported WITHOUT its bytes being
	 * checked. Removing the formats branch turns the assertions below
	 * into "2 imported, 0 mismatched" on deliberately wrong digests.
	 *
	 * Both encodings of one identity are checked, since finding only
	 * the first element would pass a single-format fixture.
	 */
	{
		char root2[] = "/tmp/cixcache-test-import2-XXXXXX";
		char m2[512];
		char f1[512];
		char f2[512];
		char f3[512];
		struct import_stats s2;

		if (mkdtemp(root2) == NULL || store_init(root2) != 0) {
			fprintf(stderr, "FAIL: cannot set up the served-shape store\n");
			return 1;
		}
		snprintf(f1, sizeof(f1), "%s/packages/zstd-1.5.4-1-x86_64.tar.gz", root2);
		snprintf(f2, sizeof(f2), "%s/packages/zstd-1.5.4-1-x86_64.cixpkg", root2);
		snprintf(f3, sizeof(f3), "%s/packages/cix-installer-9.9-1-x86_64.iso", root2);
		write_file(f1, "targz bytes");
		write_file(f2, "cixpkg bytes");
		write_file(f3, "iso bytes");

		snprintf(m2, sizeof(m2), "%s/MANIFEST.json", root2);
		write_file(m2,
		           "{\"packages\":{\"zstd-1.5.4-1-x86_64\":{\"formats\":["
		           "{\"format\":\".cixpkg\",\"file\":\"packages/zstd-1.5.4-1-x86_64.cixpkg\","
		           "\"sha256\":\"e2c5b1d02bbb5f2e4a5d7d98b47a9e1e0b8a9bb0a2b3c7a5b6b5b0cf3a0d9a91\","
		           "\"bytes\":12},"
		           "{\"format\":\".tar.gz\",\"file\":\"packages/zstd-1.5.4-1-x86_64.tar.gz\","
		           "\"sha256\":\"9a0f8f9b0e4f7a1c2d3e4f5061728394a5b6c7d8e9fa0b1c2d3e4f5061728394\","
		           "\"bytes\":11}]}},"
		           "\"installers\":{\"cix-installer-9.9-1-x86_64\":{\"formats\":["
		           "{\"format\":\".iso\",\"file\":\"packages/cix-installer-9.9-1-x86_64.iso\","
		           "\"sha256\":\"1111111111111111111111111111111111111111111111111111111111111111\","
		           "\"bytes\":9}]}}}");

		memset(&s2, 0, sizeof(s2));
		importer_run(root2, m2, 1, &s2);
		/*
		 * Both digests are deliberately wrong, so both must be
		 * MISMATCHED rather than silently imported. That is what
		 * proves the digests were FOUND: unfound ones are reported the
		 * same way an absent entry is, and a fixture with correct
		 * digests could not tell the two apart.
		 */
		/*
		 * Three, not two: the ISO counts. While only "packages" was
		 * searched, an installer matched no entry, and an artifact the
		 * manifest does not mention is imported with its bytes checked
		 * against nothing -- so a corrupted ISO entered the store
		 * silently. That is the artifact that can least afford it: a
		 * person verifies an ISO with minisign against a pinned key,
		 * and this path is upstream of the bytes that signature would
		 * be checked against.
		 */
		CHECK(s2.mismatched == 3,
		      "every format in a served manifest is found and checked, installers included");
		CHECK(s2.imported == 0, "and none is imported on a bad digest");
		store_init(root);
	}

	if (g_failures == 0)
		printf("test_import: ok\n");
	else
		printf("test_import: %d failure(s)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
