/*
 * Serving tests against a real cixcached: the contract URLs, misses,
 * traversal attempts, HEAD, and -- the one that matters most -- a
 * large-body download proving the partial-sendfile state machine
 * reassembles the file byte for byte.
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

#define PORT 7801
#define TOKEN "servetoken"

/*
 * 64 MiB. Well past any socket buffer, so the response cannot be
 * handed to the kernel in one sendfile() call -- the transfer has to
 * survive dozens of EAGAIN/EPOLLOUT round trips, which is precisely
 * the logic being tested. A multi-GB file would exercise nothing more.
 */
#define BIG_BYTES (64LL * 1024 * 1024)

static int publish(struct testserver *ts, const char *src, const char *name, const char *digest,
                   int images)
{
	char cmd[1024];
	char out[64];

	snprintf(cmd, sizeof(cmd),
	         "curl -s -o /dev/null -w '%%{http_code}' -X PUT -T '%s' "
	         "-H 'Authorization: Bearer %s' -H 'X-Cix-Sha256: %s' "
	         "'http://127.0.0.1:%d/%s%s'",
	         src, TOKEN, digest, ts->port, images ? "images/" : "", name);
	ts_capture(cmd, out, sizeof(out));
	return atoi(out);
}

int main(void)
{
	char big_path[300];
	char got[128];
	char digest[128];
	char cmd[1024];
	struct testserver ts;

	memset(&ts, 0, sizeof(ts));
	if (ts_start(&ts, PORT, TOKEN) != 0) {
		fprintf(stderr, "FAIL: could not start cixcached\n");
		return 1;
	}

	snprintf(big_path, sizeof(big_path), "%s/big.src", ts.root);
	ts_make_file(big_path, BIG_BYTES);
	if (ts_sha256(big_path, digest, sizeof(digest)) != 0) {
		fprintf(stderr, "FAIL: cannot hash fixture\n");
		ts_stop(&ts);
		return 1;
	}
	CHECK(publish(&ts, big_path, "big-1.0.tar.gz", digest, 0) == 201, "publish a large package");

	/* The package tier lives at the ROOT of base_url, not under /packages/. */
	CHECK(ts_status(PORT, "GET", "/big-1.0.tar.gz", NULL) == 200, "package served at base root");
	CHECK(ts_status(PORT, "GET", "/packages/big-1.0.tar.gz", NULL) == 404,
	      "package NOT served under /packages/");

	/*
	 * The real proof: pull the whole body back through curl -- the
	 * exact client the daemon uses -- and hash what arrives.
	 */
	snprintf(cmd, sizeof(cmd),
	         "curl -fsSL 'http://127.0.0.1:%d/big-1.0.tar.gz' | /usr/bin/sha256sum | cut -d' ' -f1",
	         PORT);
	CHECK(ts_capture(cmd, got, sizeof(got)) == 0, "large download completes");
	CHECK(strcmp(got, digest) == 0, "64 MiB round-trips byte for byte through sendfile");

	/* HEAD exists for push clients; the daemon itself never sends one. */
	CHECK(ts_status(PORT, "HEAD", "/big-1.0.tar.gz", NULL) == 200, "HEAD on a present artifact");
	CHECK(ts_status(PORT, "HEAD", "/absent-1.0.tar.gz", NULL) == 404, "HEAD on a miss");
	snprintf(cmd, sizeof(cmd),
	         "curl -sI 'http://127.0.0.1:%d/big-1.0.tar.gz' | grep -i '^X-Cix-Sha256:' "
	         "| cut -d' ' -f2 | tr -d '\\r'",
	         PORT);
	CHECK(ts_capture(cmd, got, sizeof(got)) == 0 && strcmp(got, digest) == 0,
	      "HEAD reports the digest without re-hashing");

	/* A miss is cheap and ordinary -- it just means "build from source". */
	CHECK(ts_status(PORT, "GET", "/nosuch-1.0.tar.gz", NULL) == 404, "miss is a plain 404");

	CHECK(ts_status(PORT, "GET", "/../../etc/passwd.tar.gz", NULL) == 404, "traversal refused");
	CHECK(ts_status(PORT, "GET", "/%2e%2e%2fetc%2fpasswd.tar.gz", NULL) == 404,
	      "encoded traversal refused");
	CHECK(ts_status(PORT, "GET", "/etc/passwd.tar.gz", NULL) == 404, "nested path refused");

	CHECK(ts_status(PORT, "GET", "/MANIFEST.json", NULL) == 200, "manifest is generated live");
	CHECK(ts_status(PORT, "GET", "/api/v1/status", NULL) == 200, "status endpoint");
	CHECK(ts_status(PORT, "POST", "/big-1.0.tar.gz", NULL) == 405, "unsupported method");

	/*
	 * A misconfigured registry 404s every request and every host
	 * silently rebuilds from source, so hits and misses have to be
	 * distinguishable from outside. Found live -- see
	 * LAYOUT-CORRECTION.md.
	 */
	{
		char hits[64];
		char misses[64];
		char c[600];

		snprintf(c, sizeof(c),
		         "curl -s 'http://127.0.0.1:%d/api/v1/status' | tr ',' '\n' "
		         "| grep artifact_hits | cut -d: -f2",
		         PORT);
		ts_capture(c, hits, sizeof(hits));
		snprintf(c, sizeof(c),
		         "curl -s 'http://127.0.0.1:%d/api/v1/status' | tr ',' '\n' "
		         "| grep artifact_misses | cut -d: -f2",
		         PORT);
		ts_capture(c, misses, sizeof(misses));
		CHECK(atoi(hits) > 0, "artifact hits are counted");
		CHECK(atoi(misses) > 0, "artifact misses are counted separately");
	}

	/* The activity ring, and that ?after= only returns what is newer. */
	{
		char c[600];
		char n1[64];
		char n2[64];

		snprintf(c, sizeof(c),
		         "curl -s 'http://127.0.0.1:%d/api/v1/log?after=0' | grep -o '\"seq\"' | wc -l",
		         PORT);
		ts_capture(c, n1, sizeof(n1));
		CHECK(atoi(n1) > 1, "the server log records what it has been doing");

		snprintf(c, sizeof(c),
		         "curl -s 'http://127.0.0.1:%d/api/v1/log?after=999999' | grep -o '\"text\"' "
		         "| wc -l",
		         PORT);
		ts_capture(c, n2, sizeof(n2));
		CHECK(atoi(n2) == 0, "?after= past the end returns nothing to re-show");
	}

	ts_stop(&ts);
	if (g_failures == 0)
		printf("test_serve: ok\n");
	else
		printf("test_serve: %d failure(s)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
