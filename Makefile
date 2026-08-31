#
# cix-cache -- build. Tiny C Compiler exclusively, matching the Cix
# project's own toolchain mandate (its ADR-0001): dynamic linking
# against system glibc, never -static, never TCC's bundled headers.
#
# Deliberately no -std=: TCC's default loose C99-ish mode is what the
# sibling project compiles under, and pinning a standard changes which
# glibc header branches are taken. No -O either -- TCC has no optimizer
# worth the flag. -D_FORTIFY_SOURCE=0 is mandatory, not optional:
# glibc's fortify wrappers do not work under TCC.
#
# Never add -pthread. TCC does not recognise it as a driver flag and
# mishandles it destructively -- it silently drops the source file
# argument and then fails with "undefined symbol 'main'".
#
CC := tcc
BUILD := build
CFLAGS := -Wall -Werror -D_GNU_SOURCE -D_FORTIFY_SOURCE=0 -Iinclude -I$(BUILD)
CLIENT_CFLAGS := $(CFLAGS) -Iclient/include

SERVER_SRCS := src/json.c src/http.c src/store.c src/manifest.c src/conf.c src/importer.c
CLIENT_SRCS := client/src/httpclient.c src/json.c

TESTS := $(BUILD)/test_store $(BUILD)/test_http $(BUILD)/test_serve $(BUILD)/test_push \
	$(BUILD)/test_import $(BUILD)/test_manifest $(BUILD)/test_gc

PREFIX := /opt/cixcache

.PHONY: all clean install

all: $(BUILD)/cixcached $(BUILD)/cixcachectl $(TESTS)

$(BUILD):
	mkdir -p $(BUILD)

# Regenerated on every make invocation (.PHONY, not a real file
# dependency) so cixcached always reports the commit it was actually
# built from -- a stale version string would be worse than none.
ifeq ($(strip $(CIXCACHE_VERSION)),)
VERSION_CMD = git -C $(CURDIR) describe --tags --always --dirty 2>/dev/null || echo unknown
else
VERSION_CMD = echo '$(CIXCACHE_VERSION)'
endif

.PHONY: $(BUILD)/version.h
$(BUILD)/version.h: | $(BUILD)
	@printf '#ifndef VERSION_H\n#define VERSION_H\n#define CIXCACHE_BUILD_VERSION "%s"\n#define CIXCACHE_BUILD_TIME "%s"\n#endif /* VERSION_H */\n' \
		"$$($(VERSION_CMD))" \
		"$$(date -u +%Y-%m-%dT%H:%M:%SZ)" > $@

$(BUILD)/cixcached: src/main.c $(SERVER_SRCS) $(BUILD)/version.h | $(BUILD)
	$(CC) $(CFLAGS) src/main.c $(SERVER_SRCS) -o $@

$(BUILD)/cixcachectl: cli/src/main.c $(CLIENT_SRCS) $(BUILD)/version.h | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) cli/src/main.c $(CLIENT_SRCS) -o $@

$(BUILD)/test_store: test/test_store.c src/store.c | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_http: test/test_http.c src/http.c | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_manifest: test/test_manifest.c src/manifest.c src/store.c src/json.c | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_import: test/test_import.c src/importer.c src/store.c src/json.c | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

# The integration tests drive a real cixcached over real HTTP using
# curl -- the same client the Cix daemon itself fetches with -- so they
# link nothing but the harness header.
$(BUILD)/test_serve: test/test_serve.c | $(BUILD)
	$(CC) $(CFLAGS) -Itest $< -o $@

$(BUILD)/test_push: test/test_push.c | $(BUILD)
	$(CC) $(CFLAGS) -Itest $< -o $@

$(BUILD)/test_gc: test/test_gc.c | $(BUILD)
	$(CC) $(CFLAGS) -Itest $< -o $@

#
# The Cix repository deliberately has no install target -- it deploys
# through its own pkg recipe system and A/B image slots. cix-cache has
# no such machinery to lean on, so it carries one, which keeps a
# deployment reproducible instead of a remembered pile of cp commands.
#
# Does not touch the store or the config: a store is operator data and
# a config holds a token, and reinstalling a binary must never overwrite
# either.
#
install: $(BUILD)/cixcached $(BUILD)/cixcachectl
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/web $(DESTDIR)$(PREFIX)/etc
	install -m 0755 $(BUILD)/cixcached $(DESTDIR)$(PREFIX)/bin/cixcached
	install -m 0755 $(BUILD)/cixcachectl $(DESTDIR)$(PREFIX)/bin/cixcachectl
	install -m 0644 web/index.html web/app.js web/style.css web/favicon.svg web/InterVariable.woff2 $(DESTDIR)$(PREFIX)/share/web/
	@echo "installed to $(DESTDIR)$(PREFIX)"

clean:
	rm -rf $(BUILD)
