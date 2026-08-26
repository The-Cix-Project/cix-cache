#include "conf.h"

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

	conf_extract(buf, "root=", c->root, sizeof(c->root));
	conf_extract(buf, "bind=", c->bind, sizeof(c->bind));
	conf_extract(buf, "web_root=", c->web_root, sizeof(c->web_root));
	conf_extract(buf, "push_token=", c->push_token, sizeof(c->push_token));
	conf_extract(buf, "pull_token=", c->pull_token, sizeof(c->pull_token));
	if (conf_extract(buf, "port=", portbuf, sizeof(portbuf)) == 0)
		c->port = atoi(portbuf);
	free(buf);
	return 0;
}
