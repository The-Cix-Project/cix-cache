#include "conf.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void conf_defaults(struct conf *c)
{
	memset(c, 0, sizeof(*c));
	snprintf(c->root, sizeof(c->root), "%s", "cache");
	snprintf(c->bind, sizeof(c->bind), "%s", "0.0.0.0");
	snprintf(c->web_root, sizeof(c->web_root), "%s", "web");
	c->port = 8080;
	/*
	 * Today's behaviour, and the only tier whose absence of a
	 * signature is dangerous on its own: an ISO is booted, so nothing
	 * downstream gets a chance to check it. See
	 * docs/adr/0012-cixpkg-and-signature-policy.md.
	 */
	snprintf(c->require_signature, sizeof(c->require_signature), "%s", ".iso");
}

int conf_parse_port(const char *s, int *out)
{
	char *end;
	long v;

	if (s == NULL || *s == '\0')
		return -1;
	errno = 0;
	v = strtol(s, &end, 10);
	if (errno != 0 || end == s || *end != '\0')
		return -1;
	if (v < 1 || v > 65535)
		return -1;
	*out = (int)v;
	return 0;
}

/*
 * Finds "<key>" at the start of a line and returns a pointer just past
 * it, or NULL. Never sourced or executed -- a pure text scan, matching
 * how the Cix daemon reads recipes.
 */
static const char *find_key_line(const char *buf, const char *key)
{
	const char *p = buf;
	size_t keylen = strlen(key);
	int at_line_start = 1;

	while (*p != '\0') {
		if (at_line_start && strncmp(p, key, keylen) == 0)
			return p + keylen;
		at_line_start = (*p == '\n');
		p++;
	}
	return NULL;
}

int conf_extract(const char *buf, const char *key, char *out, size_t out_size)
{
	const char *val = find_key_line(buf, key);
	const char *end;
	size_t len;
	char quote = '\0';

	out[0] = '\0';
	if (val == NULL)
		return -1;
	if (*val == '"' || *val == '\'') {
		quote = *val;
		val++;
	}
	end = val;
	while (*end != '\0' && *end != '\n' && (quote == '\0' || *end != quote))
		end++;
	len = (size_t)(end - val);
	while (len > 0 && (val[len - 1] == ' ' || val[len - 1] == '\t' || val[len - 1] == '\r'))
		len--;
	if (len >= out_size)
		return -1;
	memcpy(out, val, len);
	out[len] = '\0';
	return 0;
}

/*
 * Overlays one key onto an existing value, leaving it alone when the
 * key is absent. The default a caller set before conf_load() is the
 * value that stands, which is the whole contract of "overlays path
 * onto c".
 */
static void overlay(const char *buf, const char *key, char *out, size_t out_size)
{
	char tmp[CONF_PATH_MAX];

	if (out_size > sizeof(tmp))
		return;
	if (conf_extract(buf, key, tmp, sizeof(tmp)) == 0)
		snprintf(out, out_size, "%s", tmp);
}

int conf_load(struct conf *c, const char *path)
{
	char portbuf[32];
	char *buf;
	long size;
	FILE *f;

	f = fopen(path, "rb");
	if (f == NULL)
		return 0; /* absent config is not an error -- defaults stand */
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return -1;
	}
	size = ftell(f);
	if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return -1;
	}
	buf = malloc((size_t)size + 1);
	if (buf == NULL) {
		fclose(f);
		return -1;
	}
	if (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size) {
		free(buf);
		fclose(f);
		return -1;
	}
	buf[size] = '\0';
	fclose(f);

	/*
	 * Each overlaid only when the key is actually present. Assigning
	 * unconditionally blanked every default the file did not mention
	 * -- conf_extract() empties its output before deciding the key is
	 * absent -- so a config setting only port= came back with no root
	 * and no web_root, and conf_defaults() was the source of truth
	 * only for a config file that did not exist at all.
	 */
	overlay(buf, "root=", c->root, sizeof(c->root));
	overlay(buf, "bind=", c->bind, sizeof(c->bind));
	overlay(buf, "web_root=", c->web_root, sizeof(c->web_root));
	overlay(buf, "push_token=", c->push_token, sizeof(c->push_token));
	overlay(buf, "pull_token=", c->pull_token, sizeof(c->pull_token));
	overlay(buf, "require_signature=", c->require_signature, sizeof(c->require_signature));
	/*
	 * A port the file states and the parser cannot use is fatal to the
	 * load, not silently ignored: the caller's default would then be
	 * what gets served, on an address nobody configured.
	 */
	if (conf_extract(buf, "port=", portbuf, sizeof(portbuf)) == 0 &&
	    conf_parse_port(portbuf, &c->port) != 0) {
		fprintf(stderr, "cixcached: %s: port '%s' is not a number in 1-65535\n", path, portbuf);
		free(buf);
		return -1;
	}
	free(buf);
	return 0;
}
