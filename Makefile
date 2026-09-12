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
	$(BUILD)/test_import $(BUILD)/test_manifest $(BUILD)/test_gc $(BUILD)/test_conf

PREFIX := /opt/cixcache

.PHONY: all clean install test

all: $(BUILD)/cixcached $(BUILD)/cix-cache $(TESTS)

$(BUILD):
	mkdir -p $(BUILD)

# Regenerated on every make invocation (.PHONY, not a real file
# dependency) so cixcached always reports the commit it was actually
# built from -- a stale version string would be worse than none.
#
# The identity is a CANONICAL ARTIFACT NAME, the same grammar this
# store enforces on everything it holds: <name>-<version>-<release>-<arch>
# (ADR-0007, ADR-0008). `git describe` produced "v2.17.1-7-g00f0b63",
# which the store's own parser reads as one opaque version with no
# release and no architecture -- so the daemon was the one thing in the
# system not describable by the rules it enforces.
#
#   version   the release tag, verbatim, the way a package carries
#             upstream's version
#   release   the packaging revision OF that version: 1 at the tag, and
#             one more for each commit past it, which is exactly what a
#             release number means for a package whose upstream has not
#             moved
#   arch      uname -m, spelled as the store spells it
#
# A dirty tree is NOT folded into the name. It is not part of an
# artifact's identity, and bending it in would put "dirty" where the
# architecture goes; it is reported as its own fact instead.
.PHONY: $(BUILD)/version.h
$(BUILD)/version.h: | $(BUILD)
	@set -e; \
	arch=$$(uname -m); \
	dirty=0; \
	if [ -n "$(strip $(CIXCACHE_VERSION))" ]; then \
		ident='$(strip $(CIXCACHE_VERSION))'; \
	elif tag=$$(git -C $(CURDIR) describe --tags --abbrev=0 2>/dev/null); then \
		past=$$(git -C $(CURDIR) rev-list --count "$$tag"..HEAD); \
		git -C $(CURDIR) diff-index --quiet HEAD -- 2>/dev/null || dirty=1; \
		ident="cix-cache-$$tag-$$((past + 1))-$$arch"; \
	elif git -C $(CURDIR) rev-parse --git-dir >/dev/null 2>&1; then \
		past=$$(git -C $(CURDIR) rev-list --count HEAD 2>/dev/null || echo 0); \
		git -C $(CURDIR) diff-index --quiet HEAD -- 2>/dev/null || dirty=1; \
		ident="cix-cache-v0.0.0-$$((past + 1))-$$arch"; \
	else \
		ident="cix-cache-v0.0.0-1-$$arch"; \
	fi; \
	printf '#ifndef VERSION_H\n#define VERSION_H\n#define CIXCACHE_BUILD_VERSION "%s"\n#define CIXCACHE_BUILD_TIME "%s"\n#define CIXCACHE_BUILD_DIRTY %s\n#endif /* VERSION_H */\n' \
		"$$ident" "$$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$$dirty" > $@

$(BUILD)/cixcached: src/main.c $(SERVER_SRCS) $(BUILD)/version.h | $(BUILD)
	$(CC) $(CFLAGS) src/main.c $(SERVER_SRCS) -o $@

$(BUILD)/cix-cache: cli/src/main.c $(CLIENT_SRCS) $(BUILD)/version.h | $(BUILD)
	$(CC) $(CLIENT_CFLAGS) cli/src/main.c $(CLIENT_SRCS) -o $@

# Depends on version.h: test_build_identity() asserts the daemon's own
# build identity is a name this store would accept, which is a claim
# about the generated header and not only about the parser.
$(BUILD)/test_store: test/test_store.c src/store.c $(BUILD)/version.h | $(BUILD)
	$(CC) $(CFLAGS) test/test_store.c src/store.c -o $@

$(BUILD)/test_http: test/test_http.c src/http.c | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_conf: test/test_conf.c src/conf.c | $(BUILD)
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
# Runs every test binary and stops at the first failure.
#
# The list comes from TESTS, so it cannot drift from what is built --
# README carried its own copy of it for a while and was missing
# test_conf until somebody noticed (#17).
#
# `set -e` per recipe line is not enough: the loop is one line, so
# without the explicit exit a failing test is followed by the next one
# and make sees only the last exit status. That is the bug this target
# replaces -- the documented shell loop reported success when an early
# test had failed.
#
test: $(TESTS)
	@for t in $(TESTS); do \
		./$$t || exit 1; \
	done
	@echo "all tests passed"

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
install: $(BUILD)/cixcached $(BUILD)/cix-cache
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/web $(DESTDIR)$(PREFIX)/etc
	install -m 0755 $(BUILD)/cixcached $(DESTDIR)$(PREFIX)/bin/cixcached
	install -m 0755 $(BUILD)/cix-cache $(DESTDIR)$(PREFIX)/bin/cix-cache
	# The old name keeps working, as a symlink rather than a second copy.
	# `cix cache <verb>` finds cix-cache on PATH the way git finds git-foo,
	# so the binary has to carry that name; nothing about the tool changed.
	ln -sf cix-cache $(DESTDIR)$(PREFIX)/bin/cixcachectl
	install -m 0644 web/index.html web/app.js web/style.css web/favicon.svg web/InterVariable.woff2 $(DESTDIR)$(PREFIX)/share/web/
	@echo "installed to $(DESTDIR)$(PREFIX)"

clean:
	rm -rf $(BUILD)
