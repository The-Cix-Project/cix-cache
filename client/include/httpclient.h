/*
 * The HTTP client `cix cache` talks to cixcached with.
 *
 * This used to be a verbatim copy of the Cix repository's
 * client/src/httpclient.c. That copy took IPv4 literals only and spoke
 * no TLS, so it could not reach https://cache.uk.home.arpa (#23), and its
 * rule was "fix upstream first". Upstream had neither and nothing in
 * progress (cix#726, filed 2026-11-22), so cix-cache owns its client
 * now. The flag and variable names follow what cix#726 proposes for
 * cixctl, `--ca-file` and a *_CA_FILE variable, so the two tools agree
 * if that lands.
 *
 * One request per connection, which is all cixcached serves.
 */
#ifndef HTTPCLIENT_H
#define HTTPCLIENT_H

#include "json.h"

#include <stddef.h>
#include <sys/types.h>

#define CIX_CLIENT_HOST_MAX 256
#define CIX_CLIENT_PATH_MAX 512

struct cix_client {
	char host[CIX_CLIENT_HOST_MAX]; /* a name or an address literal */
	int port;
	int tls;
	/*
	 * PEM CA bundle that verifies the server, for a site CA that is not
	 * in the system store. Empty: the system's default trust store.
	 */
	char ca_file[CIX_CLIENT_PATH_MAX];
};

#define CIX_CONTENT_TYPE_MAX 64

struct cix_response {
	int status;
	char content_type[CIX_CONTENT_TYPE_MAX]; /* empty if the response had none */
	char *body;                              /* NUL-terminated; NULL if empty */
	size_t body_len;
	struct json_value *json; /* NULL if the body was empty or not JSON */
};

/* Plain HTTP to host:port. */
void cix_client_init(struct cix_client *c, const char *host, int port);

/*
 * From a server URL: http://HOST[:PORT] or https://HOST[:PORT], with an
 * optional trailing "/". The port defaults to 80 or 443. HOST may be a
 * name, an IPv4 literal, or an IPv6 literal in brackets. Anything else,
 * such as a path, a user or an unknown scheme, is refused rather than
 * dropped. Returns 0, or -1 with the reason in err.
 */
int cix_client_parse_server(struct cix_client *c, const char *url, char *err, size_t err_size);

/*
 * The reason the last failed call in this process failed, for a
 * message the person running the command can act on, such as a
 * certificate that does not verify. Empty if nothing has failed.
 */
const char *cix_client_last_error(void);

/*
 * One connection, for a caller that streams its own request body;
 * `cix cache publish` sends gigabytes and cannot hold them as a string.
 * With TLS, the handshake and verification are done before this
 * returns: the chain against ca_file or the system store, and the name
 * or address against the certificate. Returns NULL on failure, with
 * the reason in cix_client_last_error().
 */
struct cix_conn;

struct cix_conn *cix_conn_open(const struct cix_client *c);
int cix_conn_write_all(struct cix_conn *k, const void *buf, size_t n); /* 0, or -1 */
ssize_t cix_conn_read(struct cix_conn *k, void *buf, size_t n);        /* >0, 0 at end, -1 */
void cix_conn_close(struct cix_conn *k);

/*
 * cix_conn_read(), giving up after timeout_ms: -1 with errno EAGAIN
 * then, and the connection stays usable. For waiting on an interim
 * "100 Continue" from a server that may never send one.
 */
ssize_t cix_conn_read_timeout(struct cix_conn *k, void *buf, size_t n, int timeout_ms);

/*
 * One request and its whole response. body may be NULL; when given it
 * is sent as application/json. token, when non-NULL and non-empty, is
 * sent as "Authorization: Bearer". Returns 0 with *out filled (free it
 * with cix_response_free()), whatever the HTTP status. Returns -1 for
 * a transport failure, *out untouched.
 *
 * A connection that fails is retried a few times with a short pause,
 * because a daemon mid-restart is not a daemon that is down. A
 * certificate that does not verify is not retried, because it will
 * not verify next time either. Neither is any HTTP response, including
 * an error status.
 */
int cix_client_request_with_auth(const struct cix_client *c, const char *method, const char *path,
                                 const char *token, const char *body, struct cix_response *out);

void cix_response_free(struct cix_response *r);

#endif /* HTTPCLIENT_H */
