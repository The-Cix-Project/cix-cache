/*
 * Request-parser tests. The important cases are the ones a hand-rolled
 * parser gets wrong: a request split across reads, a header section
 * that never ends, and a body that must NOT be waited for.
 */
#include "http.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, msg)                                                                           \
	do {                                                                                           \
		if (!(cond)) {                                                                             \
			fprintf(stderr, "FAIL: %s\n", msg);                                                    \
			g_failures++;                                                                          \
		}                                                                                          \
	} while (0)

static void test_simple(void)
{
	static const char *raw = "GET /bash-5.2.37.tar.gz HTTP/1.1\r\nHost: x\r\n\r\n";
	struct http_request req;
	struct http_conn c;

	http_conn_init(&c);
	CHECK(http_conn_feed(&c, raw, strlen(raw)) == 0, "feed a whole request");
	CHECK(http_conn_try_parse(&c, &req) == 1, "parse a whole request");
	CHECK(strcmp(req.method, "GET") == 0, "method");
	CHECK(strcmp(req.path, "/bash-5.2.37.tar.gz") == 0, "path");
	CHECK(req.content_length == 0, "no body");
	http_conn_free(&c);
}

static void test_split_reads(void)
{
	static const char *p1 = "PUT /x-1.0.tar.gz HTTP/1.1\r\nX-Cix-Sha256: ab";
	static const char *p2 = "cd\r\nContent-Length: 5\r\n\r\nhel";
	struct http_request req;
	struct http_conn c;
	char val[64];

	http_conn_init(&c);
	http_conn_feed(&c, p1, strlen(p1));
	CHECK(http_conn_try_parse(&c, &req) == 0, "incomplete headers need more");
	http_conn_feed(&c, p2, strlen(p2));
	CHECK(http_conn_try_parse(&c, &req) == 1, "headers complete across two reads");
	CHECK(strcmp(req.method, "PUT") == 0, "method survives the split");
	CHECK(req.content_length == 5, "content-length parsed");
	/*
	 * Completing on headers rather than on body is the whole point:
	 * a 2.7 GB upload must never accumulate in this buffer.
	 */
	CHECK(req.body_len == 3, "only the body bytes that already arrived are exposed");
	CHECK(http_find_header(req.headers, req.headers_len, "X-Cix-Sha256", val, sizeof(val)) == 4 &&
	              strcmp(val, "abcd") == 0,
	      "header value reassembled across the split");
	http_conn_free(&c);
}

static void test_header_matching(void)
{
	static const char *raw = "GET / HTTP/1.1\r\nX-Cix-Sha256-Extra: no\r\nX-Cix-Sha256: yes\r\n\r\n";
	struct http_request req;
	struct http_conn c;
	char val[64];

	http_conn_init(&c);
	http_conn_feed(&c, raw, strlen(raw));
	http_conn_try_parse(&c, &req);
	CHECK(http_find_header(req.headers, req.headers_len, "X-Cix-Sha256", val, sizeof(val)) >= 0 &&
	              strcmp(val, "yes") == 0,
	      "exact-length header match, not a prefix match");
	CHECK(http_find_header(req.headers, req.headers_len, "x-cix-sha256", val, sizeof(val)) >= 0,
	      "header lookup is case-insensitive");
	CHECK(http_find_header(req.headers, req.headers_len, "Absent", val, sizeof(val)) < 0,
	      "absent header reports absent");
	http_conn_free(&c);
}

static void test_oversized_headers(void)
{
	char blob[1024];
	struct http_conn c;
	int rc = 0;
	int i;

	memset(blob, 'a', sizeof(blob));
	http_conn_init(&c);
	/*
	 * A client that never sends the blank line must not be able to
	 * grow the buffer without bound.
	 */
	for (i = 0; i < 64; i++) {
		rc = http_conn_feed(&c, blob, sizeof(blob));
		if (rc != 0)
			break;
	}
	CHECK(rc != 0, "header section is capped");
	http_conn_free(&c);
}

static void test_path_decode(void)
{
	char path[64];

	snprintf(path, sizeof(path), "%s", "/a%2Db.tar.gz");
	CHECK(http_path_decode(path) == 0 && strcmp(path, "/a-b.tar.gz") == 0, "percent decoding");
	snprintf(path, sizeof(path), "%s", "/bad%zz");
	CHECK(http_path_decode(path) != 0, "malformed escape rejected");
}

int main(void)
{
	test_simple();
	test_split_reads();
	test_header_matching();
	test_oversized_headers();
	test_path_decode();
	if (g_failures == 0)
		printf("test_http: ok\n");
	else
		printf("test_http: %d failure(s)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
