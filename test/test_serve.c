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

static int publish(struct testserver *ts, const char *src, const char *name, const char *digest)
{
	char cmd[1024];
	char out[64];

	snprintf(cmd, sizeof(cmd),
	         "curl -s -o /dev/null -w '%%{http_code}' -X PUT -T '%s' "
	         "-H 'Authorization: Bearer %s' -H 'X-Cix-Sha256: %s' 'http://127.0.0.1:%d/%s'",
	         src, TOKEN, digest, ts->port, name);
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
	CHECK(publish(&ts, big_path, "big-1.0.tar.gz", digest) == 201, "publish a large package");

	/* The package tier lives at the ROOT of base_url, not under /packages/. */
	CHECK(ts_status(PORT, "GET", "/big-1.0.tar.gz", NULL) == 200, "package served at base root");
	CHECK(ts_status(PORT, "GET", "/packages/big-1.0.tar.gz", NULL) == 404,
	      "package NOT served under /packages/");
	/* No second tier: a nested path names nothing (ADR-0006). */
	CHECK(ts_status(PORT, "GET", "/images/big-1.0.tar.gz", NULL) == 404,
	      "nothing is served under /images/");

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
	 * HEAD on a dashboard asset has to answer as GET does. A 404 where
	 * GET returns 200 reads, to a proxy or a health check, as the asset
	 * having gone missing.
	 */
	CHECK(ts_status(PORT, "GET", "/style.css", NULL) == 200, "GET a dashboard asset");
	CHECK(ts_status(PORT, "HEAD", "/style.css", NULL) == 200, "HEAD a dashboard asset");
	CHECK(ts_status(PORT, "HEAD", "/nosuch.css", NULL) == 404, "HEAD a missing asset");

	/*
	 * And a GET must still carry its body after a HEAD earlier on the
	 * SAME connection. head_only is connection state, so without a
	 * per-request reset this second response comes back empty -- which
	 * is a blank dashboard, not an error anyone would see in a status
	 * code.
	 */
	snprintf(cmd, sizeof(cmd),
	         "curl -s --http1.1 -I 'http://127.0.0.1:%d/style.css' -o /dev/null "
	         "--next 'http://127.0.0.1:%d/app.js' -o /dev/null -w '%%{size_download}'",
	         PORT, PORT);
	CHECK(ts_capture(cmd, got, sizeof(got)) == 0 && atoi(got) > 0,
	      "a GET after a HEAD on one connection still sends its body");

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

	/*
	 * CIXPKG end to end (#13).
	 *
	 * The ticket's live evidence was a ROUTING failure, not a store
	 * miss: an absent .tar.gz answered {"error":"not found"} while an
	 * absent .cixpkg answered {"error":"no such endpoint"}, because
	 * is_artifact_path() asks the suffix table and the table had never
	 * heard of it -- so the request was never an artifact request at
	 * all. That discriminator is what this asserts, from both sides.
	 */
	{
		char pkg_path[300];
		char sig_path[300];
		char pkg_digest[128];
		char sig_digest[128];
		char c[700];
		char body[256];
		char ctype[128];
		char installers[64];
		char packages[64];

		snprintf(pkg_path, sizeof(pkg_path), "%s/zstd.cixpkg", ts.root);
		ts_make_file(pkg_path, 4096);
		CHECK(ts_sha256(pkg_path, pkg_digest, sizeof(pkg_digest)) == 0, "hash the cixpkg fixture");

		/* Absent, but now recognised: a store miss, not a routing miss. */
		snprintf(c, sizeof(c), "curl -s 'http://127.0.0.1:%d/nosuch-1.0-1-x86_64.cixpkg'", PORT);
		ts_capture(c, body, sizeof(body));
		CHECK(strstr(body, "not found") != NULL && strstr(body, "no such endpoint") == NULL,
		      "an absent .cixpkg is a store miss, not an unrouted path");

		CHECK(publish(&ts, pkg_path, "zstd-1.5.7-3-x86_64.cixpkg", pkg_digest) == 201,
		      "a .cixpkg can be published");
		CHECK(ts_status(PORT, "GET", "/zstd-1.5.7-3-x86_64.cixpkg", NULL) == 200,
		      "and served from the root of base_url like any package");

		snprintf(c, sizeof(c),
		         "curl -sI 'http://127.0.0.1:%d/zstd-1.5.7-3-x86_64.cixpkg' "
		         "| grep -i '^Content-Type:' | cut -d' ' -f2- | tr -d '\r'",
		         PORT);
		CHECK(ts_capture(c, ctype, sizeof(ctype)) == 0 &&
		              strncmp(ctype, "application/octet-stream", 23) == 0,
		      "a cixpkg is typed as an opaque container, not as gzip");

		/* A signature composes with it exactly as with .tar.gz (#12). */
		snprintf(sig_path, sizeof(sig_path), "%s/zstd.cixpkg.minisig", ts.root);
		ts_make_file(sig_path, 128);
		CHECK(ts_sha256(sig_path, sig_digest, sizeof(sig_digest)) == 0, "hash the signature");
		CHECK(publish(&ts, sig_path, "zstd-1.5.7-3-x86_64.cixpkg.minisig", sig_digest) == 201,
		      "its detached signature publishes alongside it");

		/*
		 * The tier question, asserted because it is the one thing that
		 * could have gone wrong silently: store_needs_signature() is
		 * what the listing uses to count installers, so a .cixpkg that
		 * fell on the wrong side of it would be counted as something
		 * you boot. Nothing published here is an installer.
		 */
		snprintf(c, sizeof(c),
		         "curl -s 'http://127.0.0.1:%d/api/v1/status' | tr ',' '\n' "
		         "| grep '\"packages\":' | cut -d: -f2",
		         PORT);
		ts_capture(c, packages, sizeof(packages));
		snprintf(c, sizeof(c),
		         "curl -s 'http://127.0.0.1:%d/api/v1/status' | tr ',' '\n' "
		         "| grep '\"installers\"' | cut -d: -f2",
		         PORT);
		ts_capture(c, installers, sizeof(installers));
		/*
		 * The package count is asserted first so this cannot pass by
		 * reading nothing: atoi("") is 0, and an installers check on
		 * its own would be satisfied by a field that was never there.
		 */
		CHECK(atoi(packages) >= 2, "the cixpkg is in the listing alongside the tarball");
		CHECK(atoi(installers) == 0, "a .cixpkg counts as a package, never as an installer");
	}

	ts_stop(&ts);
	if (g_failures == 0)
		printf("test_serve: ok\n");
	else
		printf("test_serve: %d failure(s)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
