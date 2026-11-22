#ifndef TLS_H
#define TLS_H

#include <stddef.h>
#include <sys/types.h>

/*
 * HTTPS for cixcached, and the only file that includes OpenSSL.
 *
 * The rest of the server sees a server context and a per-connection
 * session, both as OpenSSL's own opaque struct tags, and moves bytes
 * through tls_recv()/tls_send(), which answer like read()/write() on a
 * non-blocking socket. A session that would block says which direction
 * it is waiting for, because under TLS that need not be the direction
 * the caller was using: a read can need the socket writable while the
 * handshake answers, and a write can need it readable.
 *
 * Written against the OpenSSL 3.0 API. Cix containers ship 3.0
 * (cix-recipes openssl@3.0.x); a newer development box must not leak a
 * 3.x-only call into a build that has to link there. See
 * docs/adr/0015-cixcached-serves-https.md.
 */

struct ssl_ctx_st;
struct ssl_st;

/*
 * Builds a server context from a PEM certificate chain (leaf first,
 * then any intermediates -- the shape Cix's PKI writes as tls.crt) and
 * a PEM private key, and checks that the two belong together. TLS 1.2
 * is the floor.
 *
 * On success returns the context and writes a one-line description of
 * the certificate (subject and expiry) to desc. On failure returns NULL
 * and writes the reason to err; nothing is logged here, so a caller
 * that is reloading can keep serving the old context and say why.
 */
struct ssl_ctx_st *tls_server_ctx_load(const char *cert_path, const char *key_path, char *err,
                                       size_t err_size, char *desc, size_t desc_size);

/*
 * Drops the caller's reference. Sessions made from the context hold
 * their own, so a context replaced by a reload lives exactly as long
 * as the last connection still using it.
 */
void tls_server_ctx_free(struct ssl_ctx_st *ctx);

/* A server session on a connected, non-blocking socket, or NULL. */
struct ssl_st *tls_session_new(struct ssl_ctx_st *ctx, int fd);

/*
 * Sends close_notify if the handshake finished -- once, without waiting
 * for the peer's -- and frees the session. Does not close the fd.
 */
void tls_session_free(struct ssl_st *s);

/*
 * Like read() and write() on a non-blocking socket: >0 bytes moved;
 * 0 from tls_recv() for end of stream; -1 with errno EAGAIN when it
 * would block, *want then holding the epoll event to wait for
 * (EPOLLIN or EPOLLOUT); -1 with errno EIO for anything else, a failed
 * handshake included.
 *
 * After EAGAIN from tls_send(), the next call must pass the same bytes
 * again; the session is set up to accept a different address for them.
 */
ssize_t tls_recv(struct ssl_st *s, void *buf, size_t n, unsigned *want);
ssize_t tls_send(struct ssl_st *s, const void *buf, size_t n, unsigned *want);

/*
 * True when the session holds bytes it has already taken off the
 * socket. epoll cannot see them, so a reader must drain them before
 * waiting.
 */
int tls_pending(struct ssl_st *s);

#endif /* TLS_H */
