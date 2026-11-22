#include "tls.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

/* The newest queued OpenSSL error, or what was being done when it failed. */
static void take_error(char *err, size_t err_size, const char *doing, const char *path)
{
	unsigned long e = ERR_peek_last_error();
	char why[256];

	if (e != 0)
		ERR_error_string_n(e, why, sizeof(why));
	else
		snprintf(why, sizeof(why), "%s", strerror(errno));
	ERR_clear_error();
	snprintf(err, err_size, "%s %s: %s", doing, path, why);
}

static void describe(SSL_CTX *ctx, char *desc, size_t desc_size)
{
	X509 *leaf = SSL_CTX_get0_certificate(ctx);
	char subject[256] = "?";
	char expires[32] = "?";
	struct tm tm;

	desc[0] = '\0';
	if (leaf == NULL)
		return;
	X509_NAME_oneline(X509_get_subject_name(leaf), subject, sizeof(subject));
	if (ASN1_TIME_to_tm(X509_get0_notAfter(leaf), &tm) == 1)
		strftime(expires, sizeof(expires), "%Y-%m-%dT%H:%M:%SZ", &tm);
	snprintf(desc, desc_size, "%s, expires %s", subject, expires);
}

struct ssl_ctx_st *tls_server_ctx_load(const char *cert_path, const char *key_path, char *err,
                                       size_t err_size, char *desc, size_t desc_size)
{
	SSL_CTX *ctx;

	err[0] = '\0';
	desc[0] = '\0';
	ERR_clear_error();
	ctx = SSL_CTX_new(TLS_server_method());
	if (ctx == NULL) {
		take_error(err, err_size, "creating a TLS context for", cert_path);
		return NULL;
	}
	/*
	 * libcurl's own floor with OpenSSL 3, and what cixd fetches with,
	 * so nothing a Cix host sends is refused by this.
	 */
	SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
	/*
	 * PARTIAL_WRITE so a write that fills the socket reports what it
	 * moved, the way write() does. ACCEPT_MOVING_WRITE_BUFFER because
	 * a retried write is the same bytes at a pointer that may differ.
	 * IGNORE_UNEXPECTED_EOF so a client that closes without
	 * close_notify -- which an HTTP/1.0-style "read to close" client is
	 * entitled to -- is an end of stream rather than an error.
	 */
	SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
	SSL_CTX_set_options(ctx, SSL_OP_IGNORE_UNEXPECTED_EOF | SSL_OP_NO_RENEGOTIATION);
	if (SSL_CTX_use_certificate_chain_file(ctx, cert_path) != 1) {
		take_error(err, err_size, "loading certificate", cert_path);
		SSL_CTX_free(ctx);
		return NULL;
	}
	if (SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) != 1) {
		take_error(err, err_size, "loading private key", key_path);
		SSL_CTX_free(ctx);
		return NULL;
	}
	if (SSL_CTX_check_private_key(ctx) != 1) {
		take_error(err, err_size, "matching private key", key_path);
		SSL_CTX_free(ctx);
		return NULL;
	}
	describe(ctx, desc, desc_size);
	return ctx;
}

void tls_server_ctx_free(struct ssl_ctx_st *ctx)
{
	SSL_CTX_free(ctx);
}

struct ssl_st *tls_session_new(struct ssl_ctx_st *ctx, int fd)
{
	SSL *s = SSL_new(ctx);

	if (s == NULL) {
		ERR_clear_error();
		return NULL;
	}
	if (SSL_set_fd(s, fd) != 1) {
		ERR_clear_error();
		SSL_free(s);
		return NULL;
	}
	SSL_set_accept_state(s);
	return s;
}

void tls_session_free(struct ssl_st *s)
{
	if (s == NULL)
		return;
	/*
	 * One non-blocking attempt. The response is complete before this
	 * runs, so close_notify is a courtesy that lets the client tell a
	 * finished response from a cut one; waiting for the peer's reply
	 * would hold a connection open for nothing.
	 */
	if (SSL_is_init_finished(s))
		SSL_shutdown(s);
	ERR_clear_error();
	SSL_free(s);
}

void tls_close_notify(struct ssl_st *s)
{
	if (SSL_is_init_finished(s))
		SSL_shutdown(s);
	ERR_clear_error();
}

/* Maps an SSL_read/SSL_write result onto the read()/write() contract. */
static ssize_t result(SSL *s, int rc, unsigned *want, int reading)
{
	int e;

	if (rc > 0)
		return rc;
	e = SSL_get_error(s, rc);
	switch (e) {
	case SSL_ERROR_WANT_READ:
		*want = EPOLLIN;
		errno = EAGAIN;
		return -1;
	case SSL_ERROR_WANT_WRITE:
		*want = EPOLLOUT;
		errno = EAGAIN;
		return -1;
	case SSL_ERROR_ZERO_RETURN:
		if (reading)
			return 0;
		break;
	default:
		break;
	}
	ERR_clear_error();
	errno = EIO;
	return -1;
}

ssize_t tls_recv(struct ssl_st *s, void *buf, size_t n, unsigned *want)
{
	int len = n > 0x7fffffff ? 0x7fffffff : (int)n;

	*want = 0;
	ERR_clear_error();
	return result(s, SSL_read(s, buf, len), want, 1);
}

ssize_t tls_send(struct ssl_st *s, const void *buf, size_t n, unsigned *want)
{
	int len = n > 0x7fffffff ? 0x7fffffff : (int)n;

	*want = 0;
	ERR_clear_error();
	return result(s, SSL_write(s, buf, len), want, 0);
}

int tls_pending(struct ssl_st *s)
{
	return SSL_has_pending(s);
}
