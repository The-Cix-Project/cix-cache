/*
 * Push tests: authentication, the declared-digest check, idempotence,
 * and the immutability conflict that protects a recipe version from
 * having its bytes changed underneath it. And how a push the server
 * refuses ends (#24): with an answer the client can read, never with a
 * reset that throws it away.
 */
#include "testserver.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/* A raw connection to the test server, for what curl will not show. */
static int raw_connect(int port)
{
	struct sockaddr_in a;
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	if (fd < 0)
		return -1;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons((unsigned short)port);
	a.sin_addr.s_addr = htonl(0x7f000001);
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* The status of the next response on fd within timeout_ms, or 0. */
static int raw_status(int fd, int timeout_ms)
{
	struct pollfd p;
	char buf[512];
	ssize_t n;
	int status = 0;

	p.fd = fd;
	p.events = POLLIN;
	if (poll(&p, 1, timeout_ms) != 1)
		return 0;
	n = read(fd, buf, sizeof(buf) - 1);
	if (n <= 0)
		return 0;
	buf[n] = '\0';
	if (sscanf(buf, "HTTP/1.%*d %d", &status) != 1)
		return 0;
	return status;
}

static void raw_put_header(int fd, const char *name, long long len, const char *digest,
                           const char *token, int expect)
{
	char hdr[700];

	snprintf(hdr, sizeof(hdr),
	         "PUT /%s HTTP/1.1\r\nHost: x\r\nContent-Length: %lld\r\nX-Cix-Sha256: %s\r\n"
	         "Authorization: Bearer %s\r\n%s\r\n",
	         name, len, digest, token, expect ? "Expect: 100-continue\r\n" : "");
	if (write(fd, hdr, strlen(hdr)) != (ssize_t)strlen(hdr))
		fprintf(stderr, "warning: short header write\n");
}

/*
 * #24. A push refused before its body is read used to be closed with
 * that body still unread, which makes the kernel answer with RST, and
 * an RST discards whatever response the client has not read yet. curl,
 * still uploading, saw no status at all about one time in 25.
 *
 * The flake that caused was timing, so it is not tested by repeating
 * it. What changed is deterministic: after answering, the server
 * half-closes and keeps reading, so the client can still send, and
 * then reads a clean end of stream. Before, that next write failed
 * with EPIPE or ECONNRESET every time.
 */
static void test_refusal_is_not_a_reset(void)
{
	static char chunk[65536];
	char zeros[65];
	char buf[256];
	ssize_t w;
	ssize_t n;
	int fd;

	memset(chunk, 'x', sizeof(chunk));
	memset(zeros, '0', 64);
	zeros[64] = '\0';
	fd = raw_connect(PORT);
	CHECK(fd >= 0, "connect for the refused-upload check");
	if (fd < 0)
		return;
	raw_put_header(fd, "r-1.0-1-x86_64.cixpkg", 1000000, zeros, "wrong", 0);
	if (write(fd, chunk, sizeof(chunk)) != (ssize_t)sizeof(chunk))
		fprintf(stderr, "warning: short body write\n");
	CHECK(raw_status(fd, 2000) == 401, "a refused upload is answered");
	usleep(100000);
	w = write(fd, chunk, sizeof(chunk));
	CHECK(w > 0, "and the connection is half-closed, not reset: the client may finish sending");
	/* The rest of the response -- headers and body are separate writes -- then the end. */
	do
		n = read(fd, buf, sizeof(buf));
	while (n > 0);
	CHECK(n == 0, "and then reads a clean end of stream, not a reset");
	close(fd);
}

/*
 * Writes n bytes of filler, all of it, or fails. MSG_NOSIGNAL so a
 * reset shows up as a failed write rather than killing the test.
 */
static int raw_send_filler(int fd, long long n)
{
	static char chunk[65536];

	memset(chunk, 'x', sizeof(chunk));
	while (n > 0) {
		size_t take = n > (long long)sizeof(chunk) ? sizeof(chunk) : (size_t)n;
		ssize_t w = send(fd, chunk, take, MSG_NOSIGNAL);

		if (w <= 0)
			return -1;
		n -= w;
	}
	return 0;
}

/*
 * #26. A client that writes its whole request before reading a byte
 * -- Python's http.client, or anything hand-rolled -- only hears a
 * refusal if the server reads everything it sends. The drain used to
 * stop at 4 MiB, so the rest of a larger body met a reset: the write
 * failed and the 401 was never read. 16 MiB is four times that.
 */
static void test_write_all_then_read(void)
{
	const long long len = 16LL * 1024 * 1024;
	char zeros[65];
	int fd;

	memset(zeros, '0', 64);
	zeros[64] = '\0';
	fd = raw_connect(PORT);
	CHECK(fd >= 0, "connect for the write-everything-first check");
	if (fd < 0)
		return;
	raw_put_header(fd, "w-1.0-1-x86_64.cixpkg", len, zeros, "wrong", 0);
	CHECK(raw_send_filler(fd, len) == 0,
	      "a refused 16 MiB body can be sent to the end, not cut off by a reset");
	CHECK(raw_status(fd, 2000) == 401, "and the client then reads its 401");
	close(fd);
}

/*
 * #25. A refused request whose length nobody knows: a chunked body, or
 * a request refused before it could be parsed. It is drained like any
 * other, to the client's end of stream, instead of closed on with
 * bytes unread -- the same deterministic check as #24's: after the
 * answer the client can still write, and then reads a clean end.
 */
static void unknown_length_case(const char *what, const char *head, int want)
{
	char buf[256];
	char msg[200];
	ssize_t n;
	int fd = raw_connect(PORT);

	snprintf(msg, sizeof(msg), "connect for the %s check", what);
	CHECK(fd >= 0, msg);
	if (fd < 0)
		return;
	if (write(fd, head, strlen(head)) != (ssize_t)strlen(head))
		fprintf(stderr, "warning: short header write\n");
	raw_send_filler(fd, 65536);
	snprintf(msg, sizeof(msg), "%s: refused with %d", what, want);
	CHECK(raw_status(fd, 2000) == want, msg);
	usleep(100000);
	snprintf(msg, sizeof(msg), "%s: the client may finish sending after the answer", what);
	CHECK(raw_send_filler(fd, 65536) == 0, msg);
	shutdown(fd, SHUT_WR);
	do
		n = read(fd, buf, sizeof(buf));
	while (n > 0);
	snprintf(msg, sizeof(msg), "%s: and then reads a clean end of stream, not a reset", what);
	CHECK(n == 0, msg);
	close(fd);
}

static void test_unknown_length_refusals(void)
{
	unknown_length_case("chunked upload",
	                    "PUT /c-1.0-1-x86_64.cixpkg HTTP/1.1\r\nHost: x\r\n"
	                    "Transfer-Encoding: chunked\r\nX-Cix-Sha256: "
	                    "0000000000000000000000000000000000000000000000000000000000000000"
	                    "\r\n\r\n10000\r\n",
	                    401);
	/* Authorized, with a length AND a transfer coding: refused, never half-read. */
	unknown_length_case("upload with both a length and a transfer coding",
	                    "PUT /t-1.0-1-x86_64.cixpkg HTTP/1.1\r\nHost: x\r\n"
	                    "Authorization: Bearer " TOKEN "\r\nContent-Length: 70000\r\n"
	                    "Transfer-Encoding: chunked\r\nX-Cix-Sha256: "
	                    "0000000000000000000000000000000000000000000000000000000000000000"
	                    "\r\n\r\n",
	                    411);
	/* Malformed: a length nobody can read, so how much follows is unknown too. */
	unknown_length_case("malformed request",
	                    "PUT /m-1.0-1-x86_64.cixpkg HTTP/1.1\r\nHost: x\r\n"
	                    "Content-Length: lots\r\n\r\n",
	                    400);
}

/*
 * #26's other half. Draining is limited by time, not bytes, so a
 * client that stops sending without closing must not hold the
 * connection for the whole 30-second window: five seconds of silence
 * ends it.
 *
 * The client cannot see that close by reading -- it already has the
 * end of stream from the half-close -- only by writing: a write to a
 * socket the server has closed draws a reset, and the write after that
 * fails. One byte after seven quiet seconds, then another.
 */
static void test_silent_drain_ends(void)
{
	char zeros[65];
	char buf[256];
	ssize_t n;
	int fd;

	memset(zeros, '0', 64);
	zeros[64] = '\0';
	fd = raw_connect(PORT);
	CHECK(fd >= 0, "connect for the silent-drain check");
	if (fd < 0)
		return;
	raw_put_header(fd, "s-1.0-1-x86_64.cixpkg", 10000000, zeros, "wrong", 0);
	raw_send_filler(fd, 65536);
	CHECK(raw_status(fd, 2000) == 401, "a refused upload that then goes quiet is answered");
	do
		n = read(fd, buf, sizeof(buf));
	while (n > 0);
	sleep(7);
	raw_send_filler(fd, 1);
	usleep(200000);
	CHECK(raw_send_filler(fd, 1) != 0,
	      "and is closed after five quiet seconds, not held for the whole drain window");
	close(fd);
}

/*
 * Expect: 100-continue. curl sends it by itself for bodies over 1 MB,
 * and waited a second for nothing on every large publish until it was
 * answered. A push that will be refused is refused before the body,
 * which is what lets a client hear a 401 for a file of any size.
 */
static void test_expect_continue(const char *path, const char *digest, long long len)
{
	static char buf[65536];
	FILE *f;
	size_t n;
	int fd;

	fd = raw_connect(PORT);
	CHECK(fd >= 0, "connect for the 100-continue check");
	if (fd < 0)
		return;
	raw_put_header(fd, "e-1.0-1-x86_64.cixpkg", len, digest, TOKEN, 1);
	CHECK(raw_status(fd, 500) == 100, "a push that will be accepted is answered 100 Continue");
	f = fopen(path, "rb");
	while (f != NULL && (n = fread(buf, 1, sizeof(buf), f)) > 0)
		if (write(fd, buf, n) != (ssize_t)n)
			break;
	if (f != NULL)
		fclose(f);
	CHECK(raw_status(fd, 5000) == 201, "and then publishes as usual");
	close(fd);

	fd = raw_connect(PORT);
	if (fd < 0)
		return;
	raw_put_header(fd, "e2-1.0-1-x86_64.cixpkg", len, digest, "wrong", 1);
	CHECK(raw_status(fd, 500) == 401,
	      "a push that will be refused is refused before the body, not answered 100");
	close(fd);

	fd = raw_connect(PORT);
	if (fd < 0)
		return;
	raw_put_header(fd, "e-1.0-1-x86_64.cixpkg", len, "1111111111111111111111111111111111111111111111111111111111111111", TOKEN, 1);
	CHECK(raw_status(fd, 500) == 409, "a name already holding other bytes is refused before the body");
	close(fd);
}

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
	 * #21. Deleting the ISO takes its signature with it, and the
	 * reason is the next assertion rather than tidiness: while the
	 * orphan survived, it still satisfied the gate above, so a push of
	 * DIFFERENT bytes under the same name was accepted -- 201, leaving
	 * the store serving a bootable that its own published signature
	 * fails to verify. The refusal here is the gate working on a store
	 * that no longer contains a signature for bytes it no longer has.
	 */
	CHECK(ts_status(PORT, "DELETE", "/inst-1.0-1-x86_64.iso", TOKEN) == 204,
	      "the signed ISO is deleted");
	CHECK(ts_status(PORT, "GET", "/inst-1.0-1-x86_64.iso", NULL) == 404,
	      "the ISO is gone");
	CHECK(ts_status(PORT, "GET", "/inst-1.0-1-x86_64.iso.minisig", NULL) == 404,
	      "and its signature went with it, rather than being left served");
	CHECK(put(&ts, b_path, "inst-1.0-1-x86_64.iso", b_digest, TOKEN) == 409,
	      "so re-publishing different bytes under that name is refused again");
	CHECK(ts_status(PORT, "GET", "/inst-1.0-1-x86_64.iso", NULL) == 404,
	      "and nothing was stored by the refused push");

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

	test_refusal_is_not_a_reset();
	test_expect_continue(a_path, a_digest, 200000);
	test_write_all_then_read();
	test_unknown_length_refusals();
	test_silent_drain_ends();

	ts_stop(&ts);
	if (g_failures == 0)
		printf("test_push: ok\n");
	else
		printf("test_push: %d failure(s)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
