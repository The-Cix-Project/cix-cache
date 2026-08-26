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
 */

#define CONF_PATH_MAX 512
#define CONF_TOKEN_MAX 256

struct conf {
	char root[CONF_PATH_MAX];
	char bind[64];
	int port;
	char web_root[CONF_PATH_MAX];
	char push_token[CONF_TOKEN_MAX];
	char pull_token[CONF_TOKEN_MAX];
};

void conf_defaults(struct conf *c);

/*
 * Overlays path onto c. A missing file is not an error -- it means the
 * defaults stand, the same contract the sibling project's
 * persist_read_file() carries. Returns 0, or -1 if the file exists but
 * could not be read.
 */
int conf_load(struct conf *c, const char *path);

/* Finds "<key>=" at the start of a line; quote tolerant. 0, or -1 if absent. */
int conf_extract(const char *buf, const char *key, char *out, size_t out_size);

#endif /* CONF_H */
