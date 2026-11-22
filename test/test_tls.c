/*
 * HTTPS, end to end (ADR-0015): a real cixcached serving a certificate
 * this test generates, driven by curl -- the client cixd itself fetches
 * with -- and by openssl s_client where the question is about the
 * certificate rather than the response.
 *
 * What it holds the server to:
 *   - an artifact published over HTTPS comes back byte for byte, large
 *     enough that the body crosses many TLS_CHUNKs and a slow reader
 *     forces the session to stop and resume mid-chunk;
 *   - plain_http=ro serves every read and refuses every write and every
 *     use of the push token, while the same request over HTTPS works;
 *   - nothing below TLS 1.2 is accepted;
 *   - SIGHUP swaps the certificate, and a reload that fails keeps the
 *     old one in service;
 *   - a configuration that cannot serve what it asks for does not start.
 */
#include "testserver.h"

#include <errno.h>

#define PORT 7804
#define TLS_PORT 7805
#define TOKEN "tls-test-token"

static int g_failures;

#define CHECK(cond, msg)                                                                           \
	do {                                                                                           \
		if (!(cond)) {                                                                             \
			fprintf(stderr, "FAIL: %s\n", msg);                                                    \
			g_failures++;                                                                          \
		}                                                                                          \
	} while (0)

static char g_dir[256];
static char g_crt[300];
static char g_key[300];

/* A self-signed RSA-2048 leaf for localhost: the key type Cix's PKI issues. */
static int make_cert(const char *crt, const char *key, const char *cn)
{
	char cmd[1200];

	snprintf(cmd, sizeof(cmd),
	         "openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj '/CN=%s' "
	         "-addext 'subjectAltName=DNS:localhost' -keyout '%s' -out '%s' >/dev/null 2>&1",
	         cn, key, crt);
	return system(cmd) == 0 ? 0 : -1;
}

/* curl against the HTTPS port, verifying against the test's certificate. */
static void https_cmd(char *cmd, size_t size, const char *opts, const char *path)
{
	snprintf(cmd, size,
	         "curl -s --cacert '%s' --resolve 'localhost:%d:127.0.0.1' %s "
	         "'https://localhost:%d%s'",
	         g_crt, TLS_PORT, opts, TLS_PORT, path);
}

static int https_status(const char *method, const char *path, const char *token)
{
	char opts[400];
	char cmd[1200];
	char out[64];

	snprintf(opts, sizeof(opts), "-o /dev/null -w '%%{http_code}' -X %s %s%s%s", method,
	         token != NULL ? "-H 'Authorization: Bearer " : "", token != NULL ? token : "",
	         token != NULL ? "'" : "");
	https_cmd(cmd, sizeof(cmd), opts, path);
	if (ts_capture(cmd, out, sizeof(out)) != 0 && out[0] == '\0')
		return -1;
	return atoi(out);
}

/* The subject CN the HTTPS port presents to a new connection. */
static void served_cn(char *out, size_t size)
{
	char cmd[600];

	snprintf(cmd, sizeof(cmd),
	         "openssl s_client -connect 127.0.0.1:%d -servername localhost </dev/null "
	         "2>/dev/null | openssl x509 -noout -subject -nameopt sep_multiline "
	         "| sed -n 's/^ *CN=//p'",
	         TLS_PORT);
	ts_capture(cmd, out, size);
}

/* Exit status of a cixcached run that is expected to refuse to start. */
static int refused(const char *args)
{
	char cmd[1200];
	int rc;

	snprintf(cmd, sizeof(cmd),
	         "timeout 5 build/cixcached --root='%s/refuse' --bind=127.0.0.1 %s >/dev/null 2>&1",
	         g_dir, args);
	rc = system(cmd);
	/* timeout(1) exits 124 when the server kept running: it started. */
	return WIFEXITED(rc) && WEXITSTATUS(rc) != 0 && WEXITSTATUS(rc) != 124;
}

static void test_https_round_trip(void)
{
	char src[400];
	char got[400];
	char digest[128];
	char back[128];
	char opts[800];
	char cmd[1600];
	char out[64];

	/* 5 MB: eighty TLS_CHUNKs, and far more than one socket buffer. */
	snprintf(src, sizeof(src), "%s/big.src", g_dir);
	snprintf(got, sizeof(got), "%s/big.got", g_dir);
	ts_make_file(src, 5 * 1024 * 1024);
	ts_sha256(src, digest, sizeof(digest));

	snprintf(opts, sizeof(opts),
	         "-o /dev/null -w '%%{http_code}' -X PUT -T '%s' "
	         "-H 'Authorization: Bearer %s' -H 'X-Cix-Sha256: %s'",
	         src, TOKEN, digest);
	https_cmd(cmd, sizeof(cmd), opts, "/big-1.0-1-x86_64.cixpkg");
	ts_capture(cmd, out, sizeof(out));
	CHECK(atoi(out) == 201, "a 5 MB artifact publishes over HTTPS");

	snprintf(opts, sizeof(opts), "-o '%s' -w '%%{http_code}'", got);
	https_cmd(cmd, sizeof(cmd), opts, "/big-1.0-1-x86_64.cixpkg");
	ts_capture(cmd, out, sizeof(out));
	CHECK(atoi(out) == 200, "and fetches over HTTPS");
	ts_sha256(got, back, sizeof(back));
	CHECK(strcmp(digest, back) == 0, "byte for byte");

	/*
	 * A reader slower than the server: the session fills the socket,
	 * reports EAGAIN partway through a chunk, and must resume with the
	 * same bytes. A wrong resume is a digest mismatch, not a hang.
	 */
	unlink(got);
	snprintf(opts, sizeof(opts), "--limit-rate 2M -o '%s' -w '%%{http_code}'", got);
	https_cmd(cmd, sizeof(cmd), opts, "/big-1.0-1-x86_64.cixpkg");
	ts_capture(cmd, out, sizeof(out));
	ts_sha256(got, back, sizeof(back));
	CHECK(atoi(out) == 200 && strcmp(digest, back) == 0,
	      "a rate-limited reader still gets every byte, in order");

	/* The same artifact over plain HTTP: one store, two transports. */
	snprintf(cmd, sizeof(cmd),
	         "curl -s -o '%s' -w '%%{http_code}' 'http://127.0.0.1:%d/big-1.0-1-x86_64.cixpkg'",
	         got, PORT);
	ts_capture(cmd, out, sizeof(out));
	ts_sha256(got, back, sizeof(back));
	CHECK(atoi(out) == 200 && strcmp(digest, back) == 0,
	      "the read-only plain port serves what HTTPS published");

	CHECK(https_status("HEAD", "/big-1.0-1-x86_64.cixpkg", NULL) == 200, "HEAD over HTTPS");
	CHECK(https_status("GET", "/nope-1.0-1-x86_64.cixpkg", NULL) == 404,
	      "a miss over HTTPS is still a visible 404");
	CHECK(https_status("GET", "/MANIFEST.json", NULL) == 200, "MANIFEST.json over HTTPS");
	CHECK(https_status("GET", "/", NULL) == 200, "the dashboard over HTTPS");
}

static void test_plain_read_only(void)
{
	char cmd[1200];
	char out[4096];

	CHECK(ts_status(PORT, "GET", "/api/v1/status", NULL) == 200, "status reads on the plain port");
	CHECK(ts_status(PORT, "GET", "/api/v1/artifacts", NULL) == 200, "so does the listing");
	CHECK(ts_status(PORT, "PUT", "/x-1.0-1-x86_64.cixpkg", TOKEN) == 403,
	      "a publish with the right token is refused on the read-only plain port");
	CHECK(ts_status(PORT, "DELETE", "/big-1.0-1-x86_64.cixpkg", TOKEN) == 403,
	      "so is a delete");
	CHECK(ts_status(PORT, "POST", "/api/v1/gc", TOKEN) == 403, "and a gc");
	CHECK(ts_status(PORT, "GET", "/api/v1/log", TOKEN) == 401,
	      "the token does not open the activity log in clear");
	CHECK(ts_status(PORT, "GET", "/api/v1/gc", TOKEN) == 401, "nor the gc dry run");

	snprintf(cmd, sizeof(cmd),
	         "curl -s -H 'Authorization: Bearer %s' 'http://127.0.0.1:%d/api/v1/status' "
	         "| grep -c '\"root\"'",
	         TOKEN, PORT);
	ts_capture(cmd, out, sizeof(out));
	CHECK(atoi(out) == 0, "nor does status treat a plain-port caller as the operator");

	CHECK(https_status("GET", "/api/v1/log", TOKEN) == 200,
	      "the activity log opens with the token over HTTPS");
	CHECK(https_status("GET", "/api/v1/gc", TOKEN) == 200, "as does the gc dry run");
	CHECK(https_status("DELETE", "/big-1.0-1-x86_64.cixpkg", TOKEN) == 204,
	      "and a delete with the token works over HTTPS");
}

static void test_protocol_floor(void)
{
	char cmd[1200];
	char out[64];

	/*
	 * Observed, not attributed: OpenSSL 3's default security level
	 * refuses 1.1 even without the server's explicit floor, so this
	 * passes with SSL_CTX_set_min_proto_version() removed. It guards
	 * the outcome a host relies on, not that one line (ADR-0015 s.5).
	 */
	https_cmd(cmd, sizeof(cmd), "-o /dev/null -w '%{http_code}' --tls-max 1.1", "/");
	ts_capture(cmd, out, sizeof(out));
	CHECK(atoi(out) != 200, "TLS 1.1 is refused");

	https_cmd(cmd, sizeof(cmd), "-o /dev/null -w '%{http_code}' --tlsv1.2 --tls-max 1.2", "/");
	ts_capture(cmd, out, sizeof(out));
	CHECK(atoi(out) == 200, "TLS 1.2 is accepted");

	/* Plain HTTP spoken at the TLS port fails that connection only. */
	snprintf(cmd, sizeof(cmd),
	         "curl -s -m 5 -o /dev/null -w '%%{http_code}' 'http://127.0.0.1:%d/'", TLS_PORT);
	ts_capture(cmd, out, sizeof(out));
	CHECK(atoi(out) != 200, "plain HTTP at the TLS port is not served");
	CHECK(https_status("GET", "/", NULL) == 200, "and the server carries on");
}

static void test_reload(const struct testserver *ts)
{
	char new_crt[300];
	char new_key[300];
	char cmd[1200];
	char cn[128];
	int tries;

	served_cn(cn, sizeof(cn));
	CHECK(strcmp(cn, "first") == 0, "the HTTPS port presents the configured certificate");

	snprintf(new_crt, sizeof(new_crt), "%s/new.crt", g_dir);
	snprintf(new_key, sizeof(new_key), "%s/new.key", g_dir);
	CHECK(make_cert(new_crt, new_key, "second") == 0, "generate a reissued certificate");
	/* Replaced the way a PKI should: by rename, never half-written. */
	snprintf(cmd, sizeof(cmd), "mv '%s' '%s' && mv '%s' '%s'", new_key, g_key, new_crt, g_crt);
	CHECK(system(cmd) == 0, "install the reissued certificate");
	kill(ts->pid, SIGHUP);
	for (tries = 0; tries < 40; tries++) {
		served_cn(cn, sizeof(cn));
		if (strcmp(cn, "second") == 0)
			break;
		usleep(50000);
	}
	CHECK(strcmp(cn, "second") == 0, "SIGHUP swaps the certificate without a restart");

	/* A reissue gone wrong: the old certificate must stay in service. */
	snprintf(cmd, sizeof(cmd), "printf 'not a certificate\\n' > '%s'", g_crt);
	CHECK(system(cmd) == 0, "break the certificate file");
	kill(ts->pid, SIGHUP);
	usleep(300000);
	served_cn(cn, sizeof(cn));
	CHECK(strcmp(cn, "second") == 0, "a failed reload keeps serving the last good certificate");
	CHECK(kill(ts->pid, 0) == 0, "and the server is still running");
}

static void test_refusals(void)
{
	char args[1200];

	snprintf(args, sizeof(args), "--port=7806 --tls-port=7807");
	CHECK(refused(args), "a tls_port with no certificate does not start");
	snprintf(args, sizeof(args), "--port=7806 --tls-port=7807 --tls-cert='%s/missing.crt' "
	                             "--tls-key='%s/missing.key'",
	         g_dir, g_dir);
	CHECK(refused(args), "a certificate that cannot be loaded does not start");
	snprintf(args, sizeof(args), "--port=7806 --tls-port=7807 --tls-cert='%s/new.key' "
	                             "--tls-key='%s/new.key'",
	         g_dir, g_dir);
	CHECK(refused(args), "nor does one that is not a certificate");
	CHECK(refused("--port=7806 --plain-http=off"), "plain_http=off with no HTTPS does not start");
	CHECK(refused("--port=7806 --plain-http=readonly"), "a misspelled plain_http does not start");
}

static void test_plain_off(void)
{
	char crt[300];
	char key[300];
	char a1[400];
	char a2[400];
	struct testserver ts;
	const char *extra[] = { "--tls-port=7807", a1, a2, "--plain-http=off", NULL };

	snprintf(crt, sizeof(crt), "%s/off.crt", g_dir);
	snprintf(key, sizeof(key), "%s/off.key", g_dir);
	CHECK(make_cert(crt, key, "off") == 0, "generate a certificate for the HTTPS-only server");
	snprintf(a1, sizeof(a1), "--tls-cert=%s", crt);
	snprintf(a2, sizeof(a2), "--tls-key=%s", key);
	memset(&ts, 0, sizeof(ts));
	CHECK(ts_start_with(&ts, 7806, TOKEN, extra, 7807) == 0, "an HTTPS-only server starts");
	CHECK(!ts_port_open(7806), "and plain_http=off opens no plain port");
	ts_stop(&ts);
}

int main(void)
{
	char a1[400];
	char a2[400];
	char port_arg[32];
	struct testserver ts;
	const char *extra[] = { port_arg, a1, a2, "--plain-http=ro", NULL };
	char tmpl[] = "/tmp/cixcache-tls-XXXXXX";

	if (mkdtemp(tmpl) == NULL) {
		fprintf(stderr, "FAIL: no scratch directory: %s\n", strerror(errno));
		return 1;
	}
	snprintf(g_dir, sizeof(g_dir), "%s", tmpl);
	snprintf(g_crt, sizeof(g_crt), "%s/tls.crt", g_dir);
	snprintf(g_key, sizeof(g_key), "%s/tls.key", g_dir);
	if (make_cert(g_crt, g_key, "first") != 0) {
		fprintf(stderr, "FAIL: could not generate a test certificate (is openssl installed?)\n");
		return 1;
	}
	snprintf(port_arg, sizeof(port_arg), "--tls-port=%d", TLS_PORT);
	snprintf(a1, sizeof(a1), "--tls-cert=%s", g_crt);
	snprintf(a2, sizeof(a2), "--tls-key=%s", g_key);

	memset(&ts, 0, sizeof(ts));
	if (ts_start_with(&ts, PORT, TOKEN, extra, TLS_PORT) != 0) {
		fprintf(stderr, "FAIL: could not start cixcached with TLS\n");
		return 1;
	}
	test_https_round_trip();
	test_plain_read_only();
	test_protocol_floor();
	test_reload(&ts);
	ts_stop(&ts);

	test_refusals();
	test_plain_off();

	{
		char cmd[400];

		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
		if (system(cmd) != 0)
			fprintf(stderr, "warning: could not clean %s\n", g_dir);
	}
	if (g_failures == 0)
		printf("test_tls: ok\n");
	else
		printf("test_tls: %d failure(s)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
