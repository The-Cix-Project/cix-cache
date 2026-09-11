/*
 * Config loading is an OVERLAY, not a replacement.
 *
 * The bug these guard against was silent and total: conf_extract()
 * empties its output before it decides a key is absent, and conf_load()
 * assigned the result unconditionally, so every key a config file did
 * not mention came back empty rather than keeping its default. A file
 * setting only port= produced a server with no root and no web_root,
 * and conf_defaults() was the source of truth only for a config that
 * did not exist at all.
 *
 * It matters more since require_signature: an existing deployment's
 * config does not mention that key, and blanking it would have turned
 * "an ISO may not be published unsigned" off on every one of them.
 */
#include "conf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, msg)                                                                           \
	do {                                                                                           \
		if (!(cond)) {                                                                             \
			fprintf(stderr, "FAIL: %s\n", msg);                                                    \
			g_failures++;                                                                          \
		}                                                                                          \
	} while (0)

static int write_conf(const char *path, const char *body)
{
	FILE *f = fopen(path, "wb");

	if (f == NULL)
		return -1;
	fputs(body, f);
	fclose(f);
	return 0;
}

int main(void)
{
	char dir[] = "/tmp/cixcache-test-conf-XXXXXX";
	char path[512];
	struct conf c;

	if (mkdtemp(dir) == NULL) {
		fprintf(stderr, "FAIL: cannot create temp dir\n");
		return 1;
	}
	snprintf(path, sizeof(path), "%s/cixcache.conf", dir);

	/* The defaults themselves, which everything below is measured against. */
	conf_defaults(&c);
	CHECK(strcmp(c.root, "cache") == 0, "root defaults");
	CHECK(strcmp(c.bind, "0.0.0.0") == 0, "bind defaults");
	CHECK(strcmp(c.web_root, "web") == 0, "web_root defaults");
	CHECK(c.port == 8080, "port defaults");
	CHECK(strcmp(c.require_signature, ".iso") == 0,
	      "and an ISO may not be published unsigned unless a config says otherwise");

	/* A config that mentions one key leaves every other default standing. */
	CHECK(write_conf(path, "port=9999\n") == 0, "write a partial config");
	conf_defaults(&c);
	CHECK(conf_load(&c, path) == 0, "a partial config loads");
	CHECK(c.port == 9999, "the key it names is applied");
	CHECK(strcmp(c.root, "cache") == 0, "a key it does not name keeps its default");
	CHECK(strcmp(c.web_root, "web") == 0, "and so does every other one");
	CHECK(strcmp(c.require_signature, ".iso") == 0,
	      "the signature policy above all, since blanking it enforces nothing");

	/* Present but empty is a VALUE, and must override the default. */
	CHECK(write_conf(path, "require_signature=\n") == 0, "write an empty-valued key");
	conf_defaults(&c);
	CHECK(conf_load(&c, path) == 0, "it loads");
	CHECK(c.require_signature[0] == '\0',
	      "an empty value is a deliberate choice and overrides, unlike an absent key");

	/* What it says on the tin. */
	CHECK(write_conf(path, "root=/srv/cache\nrequire_signature=.iso,.cixpkg\nport=1\n") == 0,
	      "write a full config");
	conf_defaults(&c);
	CHECK(conf_load(&c, path) == 0, "it loads");
	CHECK(strcmp(c.root, "/srv/cache") == 0, "root overlays");
	CHECK(strcmp(c.require_signature, ".iso,.cixpkg") == 0, "and so does a list of suffixes");

	/* An absent file is not an error -- the defaults simply stand. */
	snprintf(path, sizeof(path), "%s/nothing-here.conf", dir);
	conf_defaults(&c);
	CHECK(conf_load(&c, path) == 0, "an absent config is not an error");
	CHECK(strcmp(c.root, "cache") == 0 && strcmp(c.require_signature, ".iso") == 0,
	      "and leaves the defaults exactly as they were");

	if (g_failures == 0)
		printf("test_conf: ok\n");
	else
		printf("test_conf: %d failure(s)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
