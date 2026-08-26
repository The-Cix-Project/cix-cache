#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

/*
 * HTTP/1.1 request parser and response-header formatter.
 *
 * Shaped after the Cix daemon's daemon/src/http.c and deliberately NOT
 * a verbatim copy of it, because the two servers have opposite jobs.
 * That one is a control plane: it caps a whole request at 1 MiB, holds
 * every response body in memory, and says so in its own header as a
 * scope boundary rather than an oversight. This one is a data plane
 * whose smallest artifact is 48 KB and whose largest is 2.7 GB, so:
 *
 *   - try_parse() completes as soon as the HEADERS are complete, not
 *     the body. A PUT body is streamed straight to disk by the caller
 *     and never accumulates in this buffer.
 *   - headers alone are capped (HTTP_MAX_HEADERS), so a client that
 *     never sends a blank line cannot grow the buffer without bound.
 *   - responses are formatted as a header string for the caller to
 *     write itself, because the body is a sendfile() from a blob and
 *     this code never sees it.
 *
 * Content-Length only -- no chunked transfer-encoding, no keep-alive.
 * Every response closes the connection. The only client that matters
 * is curl -fsSL, which needs none of it.
 */

#define HTTP_MAX_METHOD 8
#define HTTP_MAX_PATH 512
#define HTTP_MAX_HEADERS 8192

struct http_conn {
	char *buf;
	size_t len;
	size_t cap;
	int headers_end; /* offset just past the blank line, or -1 */
	long content_length;
	char method[HTTP_MAX_METHOD];
	char path[HTTP_MAX_PATH];
};

/*
 * A view into the owning http_conn's buffer -- body and headers point
 * into it, so neither outlives the conn nor is freed separately.
 * body/body_len are only the body bytes that happened to arrive in the
 * same read as the headers; for a streaming PUT the caller writes those
 * out first and reads the remainder straight off the socket.
 */
struct http_request {
	char method[HTTP_MAX_METHOD];
	char path[HTTP_MAX_PATH];
	long content_length;
	char *body;
	size_t body_len;
	char *headers;
	size_t headers_len;
};

void http_conn_init(struct http_conn *c);
void http_conn_free(struct http_conn *c);

/* Returns 0, or -1 if the header section exceeded HTTP_MAX_HEADERS. */
int http_conn_feed(struct http_conn *c, const char *data, size_t n);

/* 1 headers complete, 0 need more, -1 malformed. */
int http_conn_try_parse(struct http_conn *c, struct http_request *req);

/*
 * Case-insensitive, exact-length name match, so "X-Foo" never matches
 * "X-Foo-Bar". Returns the value length, or -1 when absent OR when it
 * would not fit -- callers must size out generously.
 */
long http_find_header(const char *headers, size_t headers_len, const char *name, char *out,
                      size_t out_size);

/*
 * Formats a complete response header block, blank line included.
 * extra may be NULL or additional "Key: value\r\n" lines. Returns the
 * byte count, or -1 on truncation.
 */
int http_format_header(char *out, size_t out_size, int status, const char *status_text,
                       const char *content_type, long long content_length, const char *extra);

const char *http_status_text(int status);

/* Decodes %XX in a path in place. Returns 0, or -1 on a bad escape. */
int http_path_decode(char *path);

#endif /* HTTP_H */
