/*
 * Garbage collection over the real server: an unpublished name leaves
 * its blob behind, and only a collection reclaims it.
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

#define PORT 7803
#define TOKEN "gctoken"

static int count_blobs(const struct testserver *ts)
{
	char cmd[512];
	char out[64];

	snprintf(cmd, sizeof(cmd), "ls -1 '%s/blobs' 2>/dev/null | wc -l", ts->root);
	if (ts_capture(cmd, out, sizeof(out)) != 0)
		return -1;
	return atoi(out);
}

int main(void)
{
	char src[300];
	char digest[128];
	char cmd[1200];
	char out[64];
	struct testserver ts;

	memset(&ts, 0, sizeof(ts));
	if (ts_start(&ts, PORT, TOKEN) != 0) {
		fprintf(stderr, "FAIL: could not start cixcached\n");
		return 1;
	}
	snprintf(src, sizeof(src), "%s/g.src", ts.root);
	ts_make_file(src, 100000);
	ts_sha256(src, digest, sizeof(digest));

	snprintf(cmd, sizeof(cmd),
	         "curl -s -o /dev/null -w '%%{http_code}' -X PUT -T '%s' "
	         "-H 'Authorization: Bearer %s' -H 'X-Cix-Sha256: %s' "
	         "'http://127.0.0.1:%d/g-1.0.tar.gz'",
	         src, TOKEN, digest, PORT);
	ts_capture(cmd, out, sizeof(out));
	CHECK(atoi(out) == 201, "publish a blob");
	CHECK(count_blobs(&ts) == 1, "one blob stored");

	/* A referenced blob must survive collection. */
	CHECK(ts_status(PORT, "POST", "/api/v1/gc", TOKEN) == 200, "gc runs");
	CHECK(count_blobs(&ts) == 1, "a published blob is not collected");

	CHECK(ts_status(PORT, "DELETE", "/g-1.0.tar.gz", TOKEN) == 204, "unpublish");
	CHECK(count_blobs(&ts) == 1, "unpublishing leaves the blob behind");

	/*
	 * The dry run is a GET, and needs the token since #19 -- it walks
	 * every published name and every blob to build the live set, so it
	 * is both operator information and an expensive answer to give an
	 * anonymous caller on demand.
	 */
	CHECK(ts_status(PORT, "GET", "/api/v1/gc", TOKEN) == 200, "gc dry-run is a GET");
	CHECK(ts_status(PORT, "GET", "/api/v1/gc", NULL) == 401, "and it needs a token too");
	CHECK(count_blobs(&ts) == 1, "dry-run removes nothing");

	CHECK(ts_status(PORT, "POST", "/api/v1/gc", NULL) == 401, "gc needs a token");
	CHECK(ts_status(PORT, "POST", "/api/v1/gc", TOKEN) == 200, "gc runs again");
	CHECK(count_blobs(&ts) == 0, "the orphaned blob is collected");

	ts_stop(&ts);
	if (g_failures == 0)
		printf("test_gc: ok\n");
	else
		printf("test_gc: %d failure(s)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
