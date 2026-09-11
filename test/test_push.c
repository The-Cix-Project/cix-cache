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
               const char *token)
{
	char cmd[1200];
	char out[64];

	snprintf(cmd, sizeof(cmd),
	         "curl -s -o /dev/null -w '%%{http_code}' -X PUT -T '%s' "
	         "%s%s%s -H 'X-Cix-Sha256: %s' 'http://127.0.0.1:%d/%s'",
	         src, token != NULL ? "-H 'Authorization: Bearer " : "", token != NULL ? token : "",
	         token != NULL ? "'" : "", digest, ts->port, name);
	ts_capture(cmd, out, sizeof(out));
	return atoi(out);
}

int main(void)
{
	char a_path[300];
	char b_path[300];
	char a_digest[128];
	char b_digest[128];
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
	CHECK(put(&ts, a_path, "p-1.0.tar.gz", a_digest, NULL) == 401, "unauthenticated push");
	CHECK(put(&ts, a_path, "p-1.0.tar.gz", a_digest, "wrong") == 401, "wrong token");
	CHECK(ts_status(PORT, "GET", "/p-1.0.tar.gz", NULL) == 404, "nothing was stored");

	CHECK(put(&ts, a_path, "p-1.0.tar.gz", a_digest, TOKEN) == 201, "authenticated push");
	CHECK(ts_status(PORT, "GET", "/p-1.0.tar.gz", NULL) == 200, "pushed artifact is served");

	/* Corruption is caught at the door rather than by every puller. */
	CHECK(put(&ts, b_path, "q-1.0.tar.gz", a_digest, TOKEN) == 400,
	      "body not matching the declared digest is refused");
	CHECK(ts_status(PORT, "GET", "/q-1.0.tar.gz", NULL) == 404, "refused push published nothing");

	CHECK(put(&ts, a_path, "p-1.0.tar.gz", a_digest, TOKEN) == 201,
	      "republishing identical bytes is idempotent");

	/*
	 * The invariant: a recipe version is immutable in git, so a
	 * published name may only ever mean one byte sequence.
	 */
	/*
	 * A refused push must cost nothing on disk. The body used to be
	 * adopted into blobs/ before the name was checked, so the refusal
	 * left a full unreferenced copy of the artifact behind until a gc
	 * (#7).
	 *
	 * Counted around the FIRST refusal of these bytes, which is the
	 * only one that could ever have cost anything: adopting is
	 * content-addressed, so a retry of the same rejected artifact
	 * deduplicates onto the orphan already there. One wasted copy per
	 * distinct refused artifact, not one per attempt.
	 */
	{
		char cmd[512];
		char before[64];
		char after[64];

		snprintf(cmd, sizeof(cmd), "ls '%s/blobs' | wc -l", ts.root);
		CHECK(ts_capture(cmd, before, sizeof(before)) == 0, "count blobs before the refusal");

		CHECK(put(&ts, b_path, "p-1.0.tar.gz", b_digest, TOKEN) == 409,
		      "republishing a name with different bytes conflicts");

		CHECK(ts_capture(cmd, after, sizeof(after)) == 0, "count blobs after the refusal");
		CHECK(strcmp(before, after) == 0, "a refused push leaves no blob behind to collect");
	}

	/*
	 * There is no second tier any more: a path with a directory
	 * component names nothing this registry has (ADR-0006).
	 */
	CHECK(put(&ts, a_path, "images/dev-1.0.tar.gz", a_digest, TOKEN) == 404,
	      "a nested path is not an artifact name");

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
			CHECK(put(&ts, probe, pname, pdigest, TOKEN) == 201, msg);
		}
	}

	/* DELETE unpublishes the name; the blob stays for the collector. */
	/*
	 * A bootable may not be published unsigned, and the refusal lands
	 * at header time -- before any of the body is staged, so refusing
	 * costs a header exchange rather than a multi-gigabyte upload.
	 */
	CHECK(put(&ts, a_path, "inst-1.0-1-x86_64.iso", a_digest, TOKEN) == 409,
	      "an ISO with no signature is refused");
	CHECK(ts_status(PORT, "GET", "/inst-1.0-1-x86_64.iso", NULL) == 404,
	      "and nothing was stored");
	CHECK(put(&ts, b_path, "inst-1.0-1-x86_64.iso.minisig", b_digest, TOKEN) == 201,
	      "the signature publishes on its own");
	CHECK(put(&ts, a_path, "inst-1.0-1-x86_64.iso", a_digest, TOKEN) == 201,
	      "and then the ISO is accepted");

	/*
	 * The signature is a sibling object, not a row: one artifact, and
	 * its blob must survive collection. store_gc() builds its live set
	 * from store_walk(), so filtering signatures there rather than in
	 * the listing would free every one of them.
	 */
	{
		char body[4096];
		char cmd[512];
		char before[64];
		char after[64];

		snprintf(cmd, sizeof(cmd), "ls '%s/blobs' | wc -l", ts.root);
		CHECK(ts_capture(cmd, before, sizeof(before)) == 0, "count blobs before gc");
		snprintf(cmd, sizeof(cmd),
		         "curl -sS -X POST -H 'Authorization: Bearer %s' "
		         "'http://127.0.0.1:%d/api/v1/gc' >/dev/null; ls '%s/blobs' | wc -l",
		         TOKEN, PORT, ts.root);
		CHECK(ts_capture(cmd, after, sizeof(after)) == 0, "count blobs after gc");
		CHECK(strcmp(before, after) == 0, "collection does not free a signature's blob");
		CHECK(ts_status(PORT, "GET", "/inst-1.0-1-x86_64.iso.minisig", NULL) == 200,
		      "and the signature is still served afterwards");

		snprintf(cmd, sizeof(cmd),
		         "curl -sSI 'http://127.0.0.1:%d/inst-1.0-1-x86_64.iso' "
		         "| grep -i '^content-type' | tr -d '\\r'",
		         PORT);
		CHECK(ts_capture(cmd, body, sizeof(body)) == 0 && strstr(body, "iso9660") != NULL,
		      "an ISO is typed as an ISO");
		snprintf(cmd, sizeof(cmd),
		         "curl -sSI 'http://127.0.0.1:%d/p-1.0.tar.gz' "
		         "| grep -i '^content-type' | tr -d '\\r'",
		         PORT);
		CHECK(ts_capture(cmd, body, sizeof(body)) == 0 && strstr(body, "gzip") != NULL,
		      "and a package is still typed as an archive");
	}

	/*
	 * Packages are signed additively (cix ADR-0279, #12): a signature
	 * may be published beside one, but a package is never refused for
	 * lacking one. That second half is the production-breaking case --
	 * every daemon without a signing key is still pushing packages.
	 *
	 * Note the order is the reverse of the ISO rule: the artifact
	 * first, then its signature, because the daemon signs only once a
	 * publish has been accepted.
	 */
	CHECK(put(&ts, a_path, "sp-1.0-1-x86_64.tar.gz", a_digest, TOKEN) == 201,
	      "an unsigned package publishes, as it always has");
	CHECK(put(&ts, b_path, "sp-1.0-1-x86_64.tar.gz.minisig", b_digest, TOKEN) == 201,
	      "and its signature publishes afterwards");
	CHECK(ts_status(PORT, "GET", "/sp-1.0-1-x86_64.tar.gz.minisig", NULL) == 200,
	      "the package signature is served");
	{
		char cmd[512];
		char body[4096];

		snprintf(cmd, sizeof(cmd),
		         "curl -sSI 'http://127.0.0.1:%d/sp-1.0-1-x86_64.tar.gz.minisig' "
		         "| grep -i '^content-type' | tr -d '\\r'",
		         PORT);
		CHECK(ts_capture(cmd, body, sizeof(body)) == 0 && strstr(body, "text/plain") != NULL,
		      "a package signature is typed as text, not as an archive");
	}

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
