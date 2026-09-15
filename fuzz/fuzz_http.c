/*
 * Baseline HTTP parser target: one feed, one parse.
 *
 * Deliberately exercises the *whole* input as a single feed, which is
 * the case where headers and body share the buffer -- the shape that
 * produced issue #1, where a body's bytes were counted against the
 * header cap. fuzz_http_split covers the incremental case.
 */
#include "http.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct http_conn c;
	struct http_request req;

	http_conn_init(&c);
	if (http_conn_feed(&c, (const char *)data, size) == 0) {
		int r = http_conn_try_parse(&c, &req);

		if (r == 1) {
			char buf[64];

			/*
			 * A successful parse promises these spans are in
			 * bounds; touch them so ASan says so rather than
			 * taking the promise on trust.
			 */
			if (req.headers_len > 0)
				(void)memchr(req.headers, 'x', req.headers_len);
			if (req.body_len > 0)
				(void)memchr(req.body, 'x', req.body_len);

			/* The header walker runs on attacker bytes too. */
			(void)http_find_header(req.headers, req.headers_len,
			                       "Content-Length", buf, sizeof(buf));
			(void)http_find_header(req.headers, req.headers_len,
			                       "Authorization", buf, sizeof(buf));
			(void)http_find_header(req.headers, req.headers_len,
			                       "X-Forwarded-For", buf, sizeof(buf));
		}
	}
	http_conn_free(&c);
	return 0;
}
