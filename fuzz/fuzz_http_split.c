/*
 * The HTTP parser as the reactor actually drives it: bytes arrive in
 * whatever sizes the kernel hands back from read(), and try_parse()
 * runs after every one of them.
 *
 * This is the target that can find a terminator straddling two reads,
 * a cap that measures the wrong region once the buffer has been grown
 * twice, or a re-parse of an already-parsed buffer behaving unlike the
 * first parse. fuzz_http, feeding everything at once, can find none of
 * those.
 *
 * The first input byte is taken as the chunk size rather than fuzzing
 * a separate split vector: it lets libFuzzer's mutator move a split
 * point by flipping one byte, so the split is something coverage
 * feedback can actually steer.
 */
#include "http.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct http_conn c;
	struct http_request req;
	size_t chunk;
	size_t off;

	if (size < 2)
		return 0;
	chunk = (size_t)data[0] + 1; /* 1..256 */
	data++;
	size--;

	http_conn_init(&c);
	for (off = 0; off < size; off += chunk) {
		size_t n = size - off < chunk ? size - off : chunk;

		if (http_conn_feed(&c, (const char *)data + off, n) != 0)
			break;
		if (http_conn_try_parse(&c, &req) == 1) {
			char buf[64];

			if (req.headers_len > 0)
				(void)memchr(req.headers, 'x', req.headers_len);
			if (req.body_len > 0)
				(void)memchr(req.body, 'x', req.body_len);
			(void)http_find_header(req.headers, req.headers_len,
			                       "Content-Length", buf, sizeof(buf));
			(void)http_find_header(req.headers, req.headers_len,
			                       "Transfer-Encoding", buf, sizeof(buf));
		}
	}
	/*
	 * No use of req after the loop: a later feed() can realloc the
	 * buffer, and req.headers/req.body point into it. Each iteration
	 * uses only the spans its own parse just produced -- which is the
	 * same discipline the reactor has to keep.
	 */
	http_conn_free(&c);
	return 0;
}
