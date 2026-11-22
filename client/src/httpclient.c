/*
 * The HTTP client `cix cache` talks to cixcached with: names resolved
 * with getaddrinfo(), and HTTPS through OpenSSL, verifying the server
 * the way libcurl does for cixd. See client/include/httpclient.h for
 * why this is no longer a copy of the Cix repository's client.
 */
#include "httpclient.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

static char g_last_error[512];

static void set_error(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(g_last_error, sizeof(g_last_error), fmt, ap);
	va_end(ap);
}

const char *cix_client_last_error(void)
{
	return g_last_error;
}

void cix_client_init(struct cix_client *c, const char *host, int port)
{
	memset(c, 0, sizeof(*c));
	snprintf(c->host, sizeof(c->host), "%s", host);
	c->port = port;
}

int cix_client_parse_server(struct cix_client *c, const char *url, char *err, size_t err_size)
{
	const char *p;
	const char *host_end;
	size_t host_len;
	int tls;
	int port;

	if (strncmp(url, "https://", 8) == 0) {
		tls = 1;
		p = url + 8;
	} else if (strncmp(url, "http://", 7) == 0) {
		tls = 0;
		p = url + 7;
	} else {
		snprintf(err, err_size, "'%s' is not an http:// or https:// URL", url);
		return -1;
	}
	port = tls ? 443 : 80;
	if (*p == '[') {
		/* [v6]:port -- the brackets are URL syntax, not part of the address. */
		host_end = strchr(p, ']');
		if (host_end == NULL) {
			snprintf(err, err_size, "'%s': unterminated [address]", url);
			return -1;
		}
		p++;
		host_len = (size_t)(host_end - p);
		host_end++;
	} else {
		host_end = p + strcspn(p, ":/");
		host_len = (size_t)(host_end - p);
	}
	if (host_len == 0 || host_len >= sizeof(c->host) || memchr(p, '@', host_len) != NULL) {
		snprintf(err, err_size, "'%s' does not name a server", url);
		return -1;
	}
	if (*host_end == ':') {
		char *end;
		long v;

		errno = 0;
		v = strtol(host_end + 1, &end, 10);
		if (errno != 0 || end == host_end + 1 || v < 1 || v > 65535) {
			snprintf(err, err_size, "'%s': the port is not a number in 1-65535", url);
			return -1;
		}
		port = (int)v;
		host_end = end;
	}
	/*
	 * A path would be silently dropped -- every request goes to the
	 * server's root -- so it is refused instead of half-honoured.
	 */
	if (!(host_end[0] == '\0' || (host_end[0] == '/' && host_end[1] == '\0'))) {
		snprintf(err, err_size, "'%s': give the server only, not a path", url);
		return -1;
	}
	memset(c->host, 0, sizeof(c->host));
	memcpy(c->host, p, host_len);
	c->port = port;
	c->tls = tls;
	return 0;
}

/* ---- one connection ---- */

struct cix_conn {
	int fd;
	SSL_CTX *ctx;
	SSL *ssl;
};

/* Whether a failure is worth another attempt: the network may recover, a certificate will not. */
static int g_fatal;

static int tcp_connect(const struct cix_client *c)
{
	struct addrinfo hints;
	struct addrinfo *res;
	struct addrinfo *ai;
	char portstr[16];
	int saved = 0;
	int rc;
	int fd = -1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(portstr, sizeof(portstr), "%d", c->port);
	rc = getaddrinfo(c->host, portstr, &hints, &res);
	if (rc != 0) {
		set_error("%s: %s", c->host, gai_strerror(rc));
		return -1;
	}
	/* Every address in turn: localhost is ::1 first, and the server may bind only 127.0.0.1. */
	for (ai = res; ai != NULL; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
		if (fd < 0) {
			saved = errno;
			continue;
		}
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
			break;
		saved = errno;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd < 0)
		set_error("%s:%d: %s", c->host, c->port, strerror(saved != 0 ? saved : ECONNREFUSED));
	return fd;
}

static int is_ip_literal(const char *host)
{
	unsigned char buf[sizeof(struct in6_addr)];

	return inet_pton(AF_INET, host, buf) == 1 || inet_pton(AF_INET6, host, buf) == 1;
}

static int tls_start(struct cix_conn *k, const struct cix_client *c)
{
	long vr;

	k->ctx = SSL_CTX_new(TLS_client_method());
	if (k->ctx == NULL) {
		set_error("cannot create a TLS context");
		return -1;
	}
	SSL_CTX_set_min_proto_version(k->ctx, TLS1_2_VERSION);
	SSL_CTX_set_verify(k->ctx, SSL_VERIFY_PEER, NULL);
	/*
	 * Read-to-close is how this client finds the end of a response,
	 * and a server is allowed to close without close_notify. A cut-off
	 * body is caught instead by checking it against Content-Length.
	 */
	SSL_CTX_set_options(k->ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
	if (c->ca_file[0] != '\0') {
		if (SSL_CTX_load_verify_locations(k->ctx, c->ca_file, NULL) != 1) {
			set_error("cannot load CA file %s", c->ca_file);
			g_fatal = 1;
			return -1;
		}
	} else if (SSL_CTX_set_default_verify_paths(k->ctx) != 1) {
		set_error("cannot load the system's trusted CAs");
		g_fatal = 1;
		return -1;
	}
	k->ssl = SSL_new(k->ctx);
	if (k->ssl == NULL || SSL_set_fd(k->ssl, k->fd) != 1) {
		set_error("cannot create a TLS session");
		return -1;
	}
	/*
	 * The certificate must name what was asked for: a DNS name with SNI,
	 * or an address against its IP SANs. Without this a valid
	 * certificate for any host at all would be accepted.
	 */
	if (is_ip_literal(c->host)) {
		if (X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(k->ssl), c->host) != 1) {
			set_error("cannot check %s against the certificate", c->host);
			return -1;
		}
	} else if (SSL_set_tlsext_host_name(k->ssl, c->host) != 1 || SSL_set1_host(k->ssl, c->host) != 1) {
		set_error("cannot check %s against the certificate", c->host);
		return -1;
	}
	if (SSL_connect(k->ssl) == 1)
		return 0;
	vr = SSL_get_verify_result(k->ssl);
	if (vr != X509_V_OK) {
		set_error("https://%s:%d: certificate not trusted: %s%s", c->host, c->port,
		          X509_verify_cert_error_string(vr),
		          c->ca_file[0] == '\0' ? " (a site CA needs --ca-file or CIX_CACHE_CA_FILE)" : "");
		g_fatal = 1;
	} else {
		char why[256];
		unsigned long e = ERR_peek_last_error();

		if (e != 0)
			ERR_error_string_n(e, why, sizeof(why));
		else
			snprintf(why, sizeof(why), "connection closed");
		set_error("https://%s:%d: TLS handshake failed: %s", c->host, c->port, why);
	}
	ERR_clear_error();
	return -1;
}

struct cix_conn *cix_conn_open(const struct cix_client *c)
{
	struct cix_conn *k = calloc(1, sizeof(*k));

	if (k == NULL) {
		set_error("out of memory");
		return NULL;
	}
	k->fd = tcp_connect(c);
	if (k->fd < 0) {
		free(k);
		return NULL;
	}
	if (c->tls && tls_start(k, c) != 0) {
		cix_conn_close(k);
		return NULL;
	}
	return k;
}

int cix_conn_write_all(struct cix_conn *k, const void *buf, size_t n)
{
	const char *p = buf;

	while (n > 0) {
		ssize_t w;

		if (k->ssl != NULL) {
			int len = n > 0x40000000 ? 0x40000000 : (int)n;

			w = SSL_write(k->ssl, p, len);
			if (w <= 0) {
				ERR_clear_error();
				set_error("connection closed while sending");
				return -1;
			}
		} else {
			w = write(k->fd, p, n);
			if (w < 0) {
				if (errno == EINTR)
					continue;
				set_error("sending: %s", strerror(errno));
				return -1;
			}
		}
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

ssize_t cix_conn_read(struct cix_conn *k, void *buf, size_t n)
{
	for (;;) {
		ssize_t r;

		if (k->ssl != NULL) {
			int len = n > 0x40000000 ? 0x40000000 : (int)n;

			r = SSL_read(k->ssl, buf, len);
			if (r > 0)
				return r;
			if (SSL_get_error(k->ssl, (int)r) == SSL_ERROR_ZERO_RETURN) {
				ERR_clear_error();
				return 0;
			}
			ERR_clear_error();
			set_error("connection failed while receiving");
			return -1;
		}
		r = read(k->fd, buf, n);
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0)
			set_error("receiving: %s", strerror(errno));
		return r;
	}
}

void cix_conn_close(struct cix_conn *k)
{
	if (k == NULL)
		return;
	if (k->ssl != NULL) {
		SSL_shutdown(k->ssl);
		SSL_free(k->ssl);
	}
	if (k->ctx != NULL)
		SSL_CTX_free(k->ctx);
	ERR_clear_error();
	if (k->fd >= 0)
		close(k->fd);
	free(k);
}

/* ---- one request ---- */

struct read_buf {
	char *buf;
	size_t len;
	size_t cap;
};

static int read_buf_grow(struct read_buf *rb, size_t want_extra)
{
	size_t ncap;
	char *nb;

	if (rb->len + want_extra <= rb->cap)
		return 0;
	ncap = rb->cap != 0 ? rb->cap * 2 : 4096;
	while (ncap < rb->len + want_extra)
		ncap *= 2;
	nb = realloc(rb->buf, ncap);
	if (nb == NULL)
		return -1;
	rb->buf = nb;
	rb->cap = ncap;
	return 0;
}

/*
 * The value of header name in a response's header block (status line
 * included, the blank line excluded), or an empty string.
 */
static void find_header_value(const char *headers, size_t headers_len, const char *name,
                              char *out, size_t out_size)
{
	size_t name_len = strlen(name);
	size_t i = 0;

	out[0] = '\0';
	while (i < headers_len && !(headers[i] == '\r' && i + 1 < headers_len && headers[i + 1] == '\n'))
		i++;
	if (i < headers_len)
		i += 2;
	while (i < headers_len) {
		size_t line_start = i;
		size_t line_len;
		size_t colon = 0;

		while (i < headers_len &&
		       !(headers[i] == '\r' && i + 1 < headers_len && headers[i + 1] == '\n'))
			i++;
		line_len = i - line_start;
		while (colon < line_len && headers[line_start + colon] != ':')
			colon++;
		if (colon < line_len && colon == name_len &&
		    strncasecmp(headers + line_start, name, name_len) == 0) {
			size_t vstart = colon + 1;
			size_t vlen;

			while (vstart < line_len && headers[line_start + vstart] == ' ')
				vstart++;
			vlen = line_len - vstart;
			if (vlen >= out_size)
				vlen = out_size - 1;
			memcpy(out, headers + line_start + vstart, vlen);
			out[vlen] = '\0';
			return;
		}
		i += 2;
	}
}

/* 0 on a response; -1 on a failure worth retrying; -2 on one that is not. */
static int do_one_request(const struct cix_client *c, const char *method, const char *path,
                          const char *token, const char *body, struct cix_response *out)
{
	struct read_buf rb = { NULL, 0, 0 };
	struct cix_conn *k;
	char header[2048];
	char auth_line[400];
	char clen[32];
	char *body_start;
	int header_len;
	int status;

	g_fatal = 0;
	k = cix_conn_open(c);
	if (k == NULL)
		return g_fatal ? -2 : -1;

	auth_line[0] = '\0';
	if (token != NULL && token[0] != '\0')
		snprintf(auth_line, sizeof(auth_line), "Authorization: Bearer %s\r\n", token);
	if (body != NULL)
		header_len = snprintf(header, sizeof(header),
		                      "%s %s HTTP/1.1\r\nHost: %s\r\n%sConnection: close\r\n"
		                      "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n",
		                      method, path, c->host, auth_line, strlen(body));
	else
		header_len = snprintf(header, sizeof(header),
		                      "%s %s HTTP/1.1\r\nHost: %s\r\n%sConnection: close\r\n\r\n", method,
		                      path, c->host, auth_line);
	if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
		set_error("request too long");
		cix_conn_close(k);
		return -2;
	}
	if (cix_conn_write_all(k, header, (size_t)header_len) != 0 ||
	    (body != NULL && cix_conn_write_all(k, body, strlen(body)) != 0)) {
		cix_conn_close(k);
		return -1;
	}
	for (;;) {
		ssize_t n;

		if (read_buf_grow(&rb, 4096) != 0) {
			set_error("out of memory");
			free(rb.buf);
			cix_conn_close(k);
			return -2;
		}
		n = cix_conn_read(k, rb.buf + rb.len, rb.cap - rb.len - 1);
		if (n < 0) {
			free(rb.buf);
			cix_conn_close(k);
			return -1;
		}
		if (n == 0)
			break;
		rb.len += (size_t)n;
	}
	cix_conn_close(k);
	rb.buf[rb.len] = '\0';

	if (sscanf(rb.buf, "HTTP/1.%*d %d", &status) != 1) {
		set_error("%s:%d did not answer with HTTP%s", c->host, c->port,
		          c->tls ? "" : " (is it an https:// server?)");
		free(rb.buf);
		return -1;
	}
	memset(out, 0, sizeof(*out));
	out->status = status;
	body_start = strstr(rb.buf, "\r\n\r\n");
	if (body_start != NULL) {
		size_t headers_len = (size_t)(body_start - rb.buf);
		size_t blen;

		find_header_value(rb.buf, headers_len, "Content-Type", out->content_type,
		                  sizeof(out->content_type));
		find_header_value(rb.buf, headers_len, "Content-Length", clen, sizeof(clen));
		body_start += 4;
		blen = rb.len - (size_t)(body_start - rb.buf);
		if (clen[0] != '\0' && strcmp(method, "HEAD") != 0 &&
		    strtoull(clen, NULL, 10) != (unsigned long long)blen) {
			set_error("response cut short: %zu of %s bytes", blen, clen);
			free(rb.buf);
			return -1;
		}
		if (blen > 0) {
			out->body = malloc(blen + 1);
			if (out->body != NULL) {
				memcpy(out->body, body_start, blen);
				out->body[blen] = '\0';
				out->body_len = blen;
			}
			out->json = json_parse(body_start, blen);
		}
	}
	free(rb.buf);
	return 0;
}

#define CIX_CLIENT_MAX_ATTEMPTS 3
#define CIX_CLIENT_RETRY_DELAY_MS 250

int cix_client_request_with_auth(const struct cix_client *c, const char *method, const char *path,
                                 const char *token, const char *body, struct cix_response *out)
{
	int attempt;

	g_last_error[0] = '\0';
	for (attempt = 1; attempt <= CIX_CLIENT_MAX_ATTEMPTS; attempt++) {
		int rc = do_one_request(c, method, path, token, body, out);

		if (rc == 0)
			return 0;
		if (rc == -2)
			return -1;
		if (attempt < CIX_CLIENT_MAX_ATTEMPTS)
			usleep(CIX_CLIENT_RETRY_DELAY_MS * 1000);
	}
	return -1;
}

void cix_response_free(struct cix_response *r)
{
	json_free(r->json);
	r->json = NULL;
	free(r->body);
	r->body = NULL;
	r->body_len = 0;
}
