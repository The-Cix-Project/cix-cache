#ifndef CONF_H
#define CONF_H

#include <stddef.h>

/*
 * Server configuration: a key=value text file, parsed the way the Cix
 * daemon parses recipes -- a pure line scan, never sourced as shell.
 * JSON would be the other in-family option but this file is edited by
 * operators by hand, and a config that cannot be parsed at boot is a
 * server that will not start.
 *
 *   root=/var/lib/cixcache
 *   bind=0.0.0.0
 *   port=8080
 *   web_root=web
 *   push_token=<token>
 *   pull_token=              (empty -- pull is open)
 *   require_signature=.iso   (suffixes an unsigned publish is refused for)
 *   tls_port=8443            (0 or absent -- no HTTPS)
 *   tls_cert=/etc/cix-tls/tls.crt
 *   tls_key=/etc/cix-tls/tls.key
 *   plain_http=rw            (rw, ro or off -- what port= is allowed to do)
 *
 * A key the file does not mention keeps the default conf_defaults()
 * set. This is an overlay, not a replacement.
 */

#define CONF_PATH_MAX 512
#define CONF_TOKEN_MAX 256

enum conf_plain_http { CONF_PLAIN_RW, CONF_PLAIN_RO, CONF_PLAIN_OFF };

struct conf {
	char root[CONF_PATH_MAX];
	char bind[64];
	int port;
	char web_root[CONF_PATH_MAX];
	char push_token[CONF_TOKEN_MAX];
	char pull_token[CONF_TOKEN_MAX];
	/*
	 * Which artifact suffixes may not be published unsigned, comma
	 * separated, empty for none. The store holds the authoritative
	 * copy of this policy -- see store_set_signature_policy() -- and
	 * this is only how an operator states it. Default ".iso".
	 */
	char require_signature[CONF_TOKEN_MAX];
	/*
	 * HTTPS. tls_port 0 means none. A port without a certificate and
	 * key, or a certificate without a port, is refused by
	 * conf_check(): either is a daemon that starts and does not serve
	 * what its operator asked for.
	 */
	int tls_port;
	char tls_cert[CONF_PATH_MAX];
	char tls_key[CONF_PATH_MAX];
	/*
	 * What the plain-HTTP port may do once HTTPS exists. Read-only
	 * keeps every unauthenticated pull working and accepts nothing
	 * that needs the push token, which would otherwise cross the
	 * network in clear. See docs/adr/0015-cixcached-serves-https.md.
	 */
	enum conf_plain_http plain_http;
};

void conf_defaults(struct conf *c);

/*
 * Overlays path onto c. A missing file is not an error -- it means the
 * defaults stand, the same contract the sibling project's
 * persist_read_file() carries.
 *
 * Returns 0, or -1 if the file exists but could not be read, or names
 * a value that cannot be used. A stated-but-unusable value is refused
 * rather than ignored: ignoring it would serve the caller's default on
 * an address nobody configured. The reason is written to stderr here,
 * so a caller should report that it is stopping rather than diagnose
 * it a second time.
 */
int conf_load(struct conf *c, const char *path);

/* Finds "<key>=" at the start of a line; quote tolerant. 0, or -1 if absent. */
int conf_extract(const char *buf, const char *key, char *out, size_t out_size);

/*
 * Parses a TCP port: 1-65535, with the whole string consumed, so
 * "8080x" is an error rather than 8080. Returns 0 and writes *out, or
 * -1 leaving *out alone.
 *
 * Shared by the config path and the flag path so the two cannot
 * disagree about what a valid port is. atoi() cannot do this job: it
 * has no failure value -- "banana" is 0 -- and its result was then
 * truncated into the 16-bit port field, so a daemon asked for 99999
 * bound 34463 and said it had bound 99999 (#16).
 */
int conf_parse_port(const char *s, int *out);

/*
 * Parses plain_http: "rw", "ro" or "off". Shared by the config path and
 * the flag path for the same reason conf_parse_port() is. 0, or -1
 * leaving *out alone.
 */
int conf_parse_plain_http(const char *s, enum conf_plain_http *out);

/* The spelling conf_parse_plain_http() accepts for v. */
const char *conf_plain_http_str(enum conf_plain_http v);

/*
 * The settings that are only wrong together, checked once every source
 * -- defaults, file, flags -- has been applied. 0, or -1 with the
 * reason written to stderr.
 */
int conf_check(const struct conf *c);

#endif /* CONF_H */
