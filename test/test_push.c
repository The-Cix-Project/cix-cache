/*
 * Push tests: authentication, the declared-digest check, idempotence,
 * and the immutability conflict that protects a recipe version from
 * having its bytes changed underneath it.
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

#define PORT 7802
#define TOKEN "pushtoken"

static int put(struct testserver *ts, const char *src, const char *name, const char *digest,
               const char *token, int images)
{
	char cmd[1200];
	char out[64];

	snprintf(cmd, sizeof(cmd),
	         "curl -s -o /dev/null -w '%%{http_code}' -X PUT -T '%s' "
	         "%s%s%s -H 'X-Cix-Sha256: %s' 'http://127.0.0.1:%d/%s%s'",
	         src, token != NULL ? "-H 'Authorization: Bearer " : "", token != NULL ? token : "",
	         token != NULL ? "'" : "", digest, ts->port, images ? "images/" : "", name);
	ts_capture(cmd, out, sizeof(out));
	return atoi(out);
}

int main(void)
{
	char a_path[300];
	char b_path[300];
	char a_digest[128];
	char b_digest[128];
	char image_name[256];
	struct testserver ts;

	memset(&ts, 0, sizeof(ts));
	if (ts_start(&ts, PORT, TOKEN) != 0) {
		fprintf(stderr, "FAIL: could not start cixcached\n");
		return 1;
	}
	snprintf(a_path, sizeof(a_path), "%s/a.src", ts.root);
	snprintf(b_path, sizeof(b_path), "%s/b.src", ts.root);
	ts_make_file(a_path, 200000);
	ts_make_file(b_path, 200000);
	ts_sha256(a_path, a_digest, sizeof(a_digest));
	ts_sha256(b_path, b_digest, sizeof(b_digest));

	/*
	 * Auth asymmetry: pull is open here, push is not. An open push
	 * lets anyone fill the disk or plant blobs every puller then has
	 * to reject.
	 */
	CHECK(put(&ts, a_path, "p-1.0.tar.gz", a_digest, NULL, 0) == 401, "unauthenticated push");
	CHECK(put(&ts, a_path, "p-1.0.tar.gz", a_digest, "wrong", 0) == 401, "wrong token");
	CHECK(ts_status(PORT, "GET", "/p-1.0.tar.gz", NULL) == 404, "nothing was stored");

	CHECK(put(&ts, a_path, "p-1.0.tar.gz", a_digest, TOKEN, 0) == 201, "authenticated push");
	CHECK(ts_status(PORT, "GET", "/p-1.0.tar.gz", NULL) == 200, "pushed artifact is served");

	/* Corruption is caught at the door rather than by every puller. */
	CHECK(put(&ts, b_path, "q-1.0.tar.gz", a_digest, TOKEN, 0) == 400,
	      "body not matching the declared digest is refused");
	CHECK(ts_status(PORT, "GET", "/q-1.0.tar.gz", NULL) == 404, "refused push published nothing");

	CHECK(put(&ts, a_path, "p-1.0.tar.gz", a_digest, TOKEN, 0) == 201,
	      "republishing identical bytes is idempotent");

	/*
	 * The invariant: a recipe version is immutable in git, so a
	 * published name may only ever mean one byte sequence.
	 */
	CHECK(put(&ts, b_path, "p-1.0.tar.gz", b_digest, TOKEN, 0) == 409,
	      "republishing a name with different bytes conflicts");

	/* Image names must carry a real 64-hex manifest hash. */
	snprintf(image_name, sizeof(image_name),
	         "dev-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef.tar.gz");
	CHECK(put(&ts, a_path, image_name, a_digest, TOKEN, 1) == 201, "image push");
	CHECK(put(&ts, a_path, "dev-1.0.tar.gz", a_digest, TOKEN, 1) == 400,
	      "image name without a manifest hash is refused");

	/*
	 * Issue #1's bracket, verbatim. Small bodies fit under the header
	 * cap and passed, large ones arrived in a read of their own and
	 * passed, and everything between was rejected as "request headers
	 * too large" -- so 12 of 13 artifacts in a real push run succeeded
	 * and the 48 KB one could not be published at all.
	 *
	 * Whether headers and body actually coalesce here is up to the
	 * kernel and curl, so this is a bracket rather than a proof; the
	 * deterministic half lives in test_http.
	 */
	{
		static const long sizes[] = { 1024, 16384, 49152, 65536, 131072, 262144 };
		char probe[300];
		char pname[64];
		char pdigest[128];
		size_t k;

		snprintf(probe, sizeof(probe), "%s/probe.bin", ts.root);
		for (k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
			char msg[96];

			ts_make_file(probe, sizes[k]);
			ts_sha256(probe, pdigest, sizeof(pdigest));
			snprintf(pname, sizeof(pname), "zz-probe-%ld.tar.gz", sizes[k]);
			snprintf(msg, sizeof(msg), "push a %ld byte body", sizes[k]);
			CHECK(put(&ts, probe, pname, pdigest, TOKEN, 0) == 201, msg);
		}
	}

	/* DELETE unpublishes the name; the blob stays for the collector. */
	CHECK(ts_status(PORT, "DELETE", "/p-1.0.tar.gz", NULL) == 401, "delete needs a token");
	CHECK(ts_status(PORT, "DELETE", "/p-1.0.tar.gz", TOKEN) == 204, "delete unpublishes");
	CHECK(ts_status(PORT, "GET", "/p-1.0.tar.gz", NULL) == 404, "deleted name is gone");
	CHECK(ts_status(PORT, "DELETE", "/p-1.0.tar.gz", TOKEN) == 404, "deleting twice is a miss");

	ts_stop(&ts);
	if (g_failures == 0)
		printf("test_push: ok\n");
	else
		printf("test_push: %d failure(s)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
