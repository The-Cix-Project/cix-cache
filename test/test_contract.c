/*
 * Protocol fidelity against the real Cix daemon's own arithmetic.
 *
 * The daemon derives an image artifact's URL from recipe text alone:
 * it sorts the recipe's package entries by name, joins them as
 * "name@version" with commas, and takes the sha256 of that exact
 * string -- no trailing newline, byte order by strcmp, not locale.
 * The result is the <image_version> in
 * GET <base>/images/<name>-<image_version>.tar.gz.
 *
 * If that derivation is off by so much as one byte, every image fetch
 * asks for a URL this server will never have. So it is reimplemented
 * here independently -- the same thing the Cix project's own
 * test_image_recipe.c does -- and checked against a version recorded
 * from a live host, then driven end to end through the server.
 *
 * Fixture: recipes/image/dev/2.0.0/build.sh from the Cix repository,
 * whose comment records the image version captured from 192.168.15.95.
 */
#include "testserver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, msg)                                                                           \
	do {                                                                                           \
		if (!(cond)) {                                                                             \
			fprintf(stderr, "FAIL: %s\n", msg);                                                    \
			g_failures++;                                                                          \
		}                                                                                          \
	} while (0)

#define PORT 7804
#define TOKEN "contracttoken"

static const char *DEV_IMAGE_PACKAGES =
        "bash:pinned:5.2.37 bc:pinned:1.08.1 binutils:pinned:2.42-7 bison:pinned:3.8.2-2 "
        "bzip2:pinned:1.0.8 coreutils:pinned:9.11-3 elfutils:pinned:0.192-6 flex:pinned:2.6.4-2 "
        "gawk:pinned:5.3.0-2 grep:pinned:3.11-2 libc-dev:pinned:2.36 m4:pinned:1.4.19 "
        "make:pinned:4.4.1 sed:pinned:4.9-2 tar:pinned:1.35-5 tcc:pinned:0.9.27 "
        "xz:pinned:5.8.3-2 zlib:pinned:1.3.2-3";

static const char *DEV_IMAGE_VERSION =
        "3a9b50b85a2333e0e60c80c1c441000f0bbd6b8c8f1434ae77095919b8b6f2da";

struct entry {
	char name[64];
	char version[64];
};

static int entry_cmp(const void *a, const void *b)
{
	/* strcmp on the NAME only, byte order -- never locale collation. */
	return strcmp(((const struct entry *)a)->name, ((const struct entry *)b)->name);
}

/*
 * Rebuilds the canonical manifest string from an image_packages= line
 * of "package:mode:version" tokens.
 */
static void manifest_string(const char *packages, char *out, size_t out_size)
{
	struct entry entries[64];
	char buf[2048];
	size_t pos = 0;
	int count = 0;
	char *save = NULL;
	char *tok;
	int i;

	snprintf(buf, sizeof(buf), "%s", packages);
	tok = strtok_r(buf, " \t", &save);
	while (tok != NULL && count < (int)(sizeof(entries) / sizeof(entries[0]))) {
		char *first = strchr(tok, ':');
		char *second = first != NULL ? strchr(first + 1, ':') : NULL;

		if (second != NULL) {
			snprintf(entries[count].name, sizeof(entries[count].name), "%.*s",
			         (int)(first - tok), tok);
			snprintf(entries[count].version, sizeof(entries[count].version), "%s", second + 1);
			count++;
		}
		tok = strtok_r(NULL, " \t", &save);
	}
	qsort(entries, (size_t)count, sizeof(entries[0]), entry_cmp);
	out[0] = '\0';
	for (i = 0; i < count; i++) {
		int n = snprintf(out + pos, out_size - pos, "%s%s@%s", (i > 0) ? "," : "",
		                 entries[i].name, entries[i].version);

		if (n < 0 || (size_t)n >= out_size - pos)
			break;
		pos += (size_t)n;
	}
}

/*
 * sha256 of a string with exactly strlen(s) bytes hashed -- no
 * trailing newline. The daemon does this by writing the string to a
 * temp file and running the real sha256sum, and so does this.
 */
static int hash_string(const char *s, const char *tmp_dir, char *out, size_t out_size)
{
	char path[400];
	FILE *f;

	snprintf(path, sizeof(path), "%s/.manifest_string", tmp_dir);
	f = fopen(path, "wb");
	if (f == NULL)
		return -1;
	fwrite(s, 1, strlen(s), f);
	fclose(f);
	return ts_sha256(path, out, out_size);
}

int main(void)
{
	char canonical[2048];
	char version[128];
	char artifact[300];
	char digest[128];
	char url[400];
	char name[256];
	char cmd[1200];
	char got[128];
	struct testserver ts;

	memset(&ts, 0, sizeof(ts));
	if (ts_start(&ts, PORT, TOKEN) != 0) {
		fprintf(stderr, "FAIL: could not start cixcached\n");
		return 1;
	}

	manifest_string(DEV_IMAGE_PACKAGES, canonical, sizeof(canonical));
	CHECK(strncmp(canonical, "bash@5.2.37,bc@1.08.1,binutils@2.42-7,", 37) == 0,
	      "canonical manifest string is sorted and comma-joined");
	CHECK(strstr(canonical, ",,") == NULL && canonical[strlen(canonical) - 1] != ',',
	      "no empty or trailing entries");

	if (hash_string(canonical, ts.root, version, sizeof(version)) != 0) {
		fprintf(stderr, "FAIL: cannot hash the manifest string\n");
		ts_stop(&ts);
		return 1;
	}
	/* The load-bearing assertion: our derivation matches a real host's. */
	CHECK(strcmp(version, DEV_IMAGE_VERSION) == 0,
	      "derived image version matches the one recorded from a live host");
	if (strcmp(version, DEV_IMAGE_VERSION) != 0)
		fprintf(stderr, "  derived %s\n  expected %s\n", version, DEV_IMAGE_VERSION);

	/* Now drive the URL the daemon would actually request. */
	snprintf(name, sizeof(name), "dev-%s.tar.gz", version);
	snprintf(url, sizeof(url), "/images/%s", name);
	snprintf(artifact, sizeof(artifact), "%s/rootfs.tar.gz", ts.root);
	ts_make_file(artifact, 1000000);
	ts_sha256(artifact, digest, sizeof(digest));

	snprintf(cmd, sizeof(cmd),
	         "curl -s -o /dev/null -w '%%{http_code}' -X PUT -T '%s' "
	         "-H 'Authorization: Bearer %s' -H 'X-Cix-Sha256: %s' 'http://127.0.0.1:%d%s'",
	         artifact, TOKEN, digest, PORT, url);
	ts_capture(cmd, got, sizeof(got));
	CHECK(atoi(got) == 201, "publish under the derived image name");

	/*
	 * Fetch exactly as the daemon does -- curl -fsSL, no HEAD, no
	 * index lookup -- and verify the bytes against the checksum a
	 * recipe would carry in image_artifact_sha256.
	 */
	snprintf(cmd, sizeof(cmd),
	         "curl -fsSL 'http://127.0.0.1:%d%s' | /usr/bin/sha256sum | cut -d' ' -f1", PORT, url);
	CHECK(ts_capture(cmd, got, sizeof(got)) == 0, "the derived URL resolves");
	CHECK(strcmp(got, digest) == 0, "fetched bytes verify against the recipe's checksum");

	/* One byte different in the manifest means a different, absent URL. */
	snprintf(cmd, sizeof(cmd), "%s", DEV_IMAGE_PACKAGES);
	manifest_string("bash:pinned:5.2.38 zlib:pinned:1.3.2-3", canonical, sizeof(canonical));
	hash_string(canonical, ts.root, version, sizeof(version));
	snprintf(url, sizeof(url), "/images/dev-%s.tar.gz", version);
	CHECK(ts_status(PORT, "GET", url, NULL) == 404,
	      "a changed manifest computes a different URL, which misses");

	ts_stop(&ts);
	if (g_failures == 0)
		printf("test_contract: ok\n");
	else
		printf("test_contract: %d failure(s)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
