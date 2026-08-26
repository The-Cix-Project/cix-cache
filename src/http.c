#include "http.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void http_conn_init(struct http_conn *c)
{
	memset(c, 0, sizeof(*c));
	c->headers_end = -1;
	c->content_length = -1;
}

void http_conn_free(struct http_conn *c)
{
	free(c->buf);
	c->buf = NULL;
	c->len = 0;
	c->cap = 0;
}

int http_conn_feed(struct http_conn *c, const char *data, size_t n)
{
	/*
	 * The cap only guards the header section. Once the blank line has
	 * been seen the caller drains the body itself -- straight to a
	 * file for a PUT -- and stops feeding, so a 2.7 GB upload never
	 * touches this buffer.
	 */
	if (c->headers_end < 0 && c->len + n > HTTP_MAX_HEADERS)
		return -1;
	if (c->len + n + 1 > c->cap) {
		size_t cap = c->cap != 0 ? c->cap : 4096;
		char *grown;

		while (cap < c->len + n + 1)
			cap *= 2;
		grown = realloc(c->buf, cap);
		if (grown == NULL)
			return -1;
		c->buf = grown;
		c->cap = cap;
	}
	memcpy(c->buf + c->len, data, n);
	c->len += n;
	c->buf[c->len] = '\0';
	return 0;
}

static int parse_request_line(struct http_conn *c, const char *line, size_t line_len)
{
	const char *sp1;
	const char *sp2;
	size_t method_len;
	size_t path_len;

	sp1 = memchr(line, ' ', line_len);
	if (sp1 == NULL)
		return -1;
	method_len = (size_t)(sp1 - line);
	if (method_len == 0 || method_len >= HTTP_MAX_METHOD)
		return -1;
	sp2 = memchr(sp1 + 1, ' ', line_len - method_len - 1);
	if (sp2 == NULL)
		return -1;
	path_len = (size_t)(sp2 - sp1 - 1);
	if (path_len == 0 || path_len >= HTTP_MAX_PATH)
		return -1;
	memcpy(c->method, line, method_len);
	c->method[method_len] = '\0';
	memcpy(c->path, sp1 + 1, path_len);
	c->path[path_len] = '\0';
	return 0;
}

int http_conn_try_parse(struct http_conn *c, struct http_request *req)
{
	const char *blank;
	const char *eol;
	char lenbuf[32];
	size_t headers_len;

	if (c->headers_end < 0) {
		if (c->buf == NULL)
			return 0;
		blank = strstr(c->buf, "\r\n\r\n");
		if (blank == NULL)
			return 0;
		c->headers_end = (int)(blank - c->buf) + 4;
		eol = strstr(c->buf, "\r\n");
		if (eol == NULL)
			return -1;
		if (parse_request_line(c, c->buf, (size_t)(eol - c->buf)) != 0)
			return -1;
	}
	headers_len = (size_t)c->headers_end - 4;
	c->content_length = 0;
	if (http_find_header(c->buf, headers_len, "Content-Length", lenbuf, sizeof(lenbuf)) >= 0) {
		char *end = NULL;
		long v = strtol(lenbuf, &end, 10);

		if (end == lenbuf || v < 0)
			return -1;
		c->content_length = v;
	}
	memset(req, 0, sizeof(*req));
	snprintf(req->method, sizeof(req->method), "%s", c->method);
	snprintf(req->path, sizeof(req->path), "%s", c->path);
	req->content_length = c->content_length;
	req->headers = c->buf;
	req->headers_len = headers_len;
	req->body = c->buf + c->headers_end;
	req->body_len = c->len - (size_t)c->headers_end;
	return 1;
}

long http_find_header(const char *headers, size_t headers_len, const char *name, char *out,
                      size_t out_size)
{
	size_t name_len = strlen(name);
	const char *p = headers;
	const char *end = headers + headers_len;

	/* Skip the request line; headers start after the first CRLF. */
	while (p < end && !(p[0] == '\r' && p + 1 < end && p[1] == '\n'))
		p++;
	if (p >= end)
		return -1;
	p += 2;
	while (p < end) {
		const char *eol = p;
		const char *colon;
		const char *val;
		size_t val_len;

		while (eol < end && *eol != '\r')
			eol++;
		colon = memchr(p, ':', (size_t)(eol - p));
		if (colon == NULL) {
			p = eol + 2;
			continue;
		}
		if ((size_t)(colon - p) != name_len || strncasecmp(p, name, name_len) != 0) {
			p = eol + 2;
			continue;
		}
		val = colon + 1;
		while (val < eol && (*val == ' ' || *val == '\t'))
			val++;
		val_len = (size_t)(eol - val);
		while (val_len > 0 && (val[val_len - 1] == ' ' || val[val_len - 1] == '\t'))
			val_len--;
		if (val_len >= out_size)
			return -1;
		memcpy(out, val, val_len);
		out[val_len] = '\0';
		return (long)val_len;
	}
	return -1;
}

const char *http_status_text(int status)
{
	switch (status) {
	case 200:
		return "OK";
	case 201:
		return "Created";
	case 204:
		return "No Content";
	case 400:
		return "Bad Request";
	case 401:
		return "Unauthorized";
	case 404:
		return "Not Found";
	case 405:
		return "Method Not Allowed";
	case 409:
		return "Conflict";
	case 411:
		return "Length Required";
	case 413:
		return "Payload Too Large";
	case 500:
		return "Internal Server Error";
	case 503:
		return "Service Unavailable";
	}
	return "Error";
}

int http_format_header(char *out, size_t out_size, int status, const char *status_text,
                       const char *content_type, long long content_length, const char *extra)
{
	int n;

	n = snprintf(out, out_size,
	             "HTTP/1.1 %d %s\r\n"
	             "Content-Type: %s\r\n"
	             "Content-Length: %lld\r\n"
	             "Connection: close\r\n"
	             "%s"
	             "\r\n",
	             status, status_text, content_type, content_length, extra != NULL ? extra : "");
	if (n < 0 || (size_t)n >= out_size)
		return -1;
	return n;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

int http_path_decode(char *path)
{
	char *r = path;
	char *w = path;

	while (*r != '\0') {
		if (*r == '%') {
			int hi = hexval(r[1]);
			int lo = hi >= 0 ? hexval(r[2]) : -1;

			if (lo < 0)
				return -1;
			*w++ = (char)((hi << 4) | lo);
			r += 3;
			continue;
		}
		*w++ = *r++;
	}
	*w = '\0';
	return 0;
}
