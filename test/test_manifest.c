/*
 * MANIFEST.json is generated from the tree on every request rather
 * than stored, so it cannot drift from the bytes. These tests check
 * the shape matches the hand-written manifest the export shipped
 * with, since recipe authors read it.
 */
#include "json.h"
#include "manifest.h"
#include "store.h"

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

static const char *HEX64 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

static void publish_fixture(const char *root, const char *name, const char *content)
{
	char digest[STORE_SHA256_MAX];
	char tmp[512];
	FILE *f;

	snprintf(tmp, sizeof(tmp), "%s/tmp/fixture", root);
	f = fopen(tmp, "wb");
	if (f == NULL)
		return;
	fputs(content, f);
	fclose(f);
	if (store_hash_file(tmp, digest, sizeof(digest)) != 0)
		return;
	store_blob_adopt(tmp, digest);
	store_publish(name, digest);
}

int main(void)
{
	char root[] = "/tmp/cixcache-test-manifest-XXXXXX";
	const struct json_value *entry;
	const struct json_value *sec;
	struct json_value *parsed;
	struct json_writer w;
	if (mkdtemp(root) == NULL || store_init(root) != 0) {
		fprintf(stderr, "FAIL: cannot set up store\n");
		return 1;
	}
	publish_fixture(root, "bash-5.2.37-2.tar.gz", "package bytes");

	jw_init(&w);
	manifest_write_json(&w);
	parsed = json_parse(w.buf, w.len);
	CHECK(parsed != NULL, "generated manifest is valid JSON");
	if (parsed == NULL) {
		jw_free(&w);
		return 1;
	}

	sec = json_object_get(parsed, "packages");
	CHECK(sec != NULL && sec->type == JSON_OBJECT, "packages section present");
	entry = json_object_get(sec, "bash-5.2.37-2");
	CHECK(entry != NULL, "package keyed by name-version, without the .tar.gz suffix");
	CHECK(entry != NULL &&
	              strcmp(json_as_string(json_object_get(entry, "file")),
	                     "packages/bash-5.2.37-2.tar.gz") == 0,
	      "package file path");
	CHECK(entry != NULL && json_as_number(json_object_get(entry, "bytes")) == 13,
	      "package byte count comes from the blob");

	CHECK(json_object_get(parsed, "images") == NULL,
	      "no images section -- packages are the only tier");

	json_free(parsed);
	jw_free(&w);

	/*
	 * The tier survives a signature policy that covers packages (#15).
	 *
	 * This is the failure the split of store_needs_signature() from
	 * store_is_installer() exists to prevent, and it is worth an
	 * assertion in the manifest's own tests because the manifest is
	 * where it would have done real damage: the sections are split on
	 * the tier, and while the two questions were one function,
	 * requiring a signature for .cixpkg would have moved every
	 * .cixpkg out of "packages" -- the section a Cix daemon resolves
	 * name@version from -- and into "installers", where nothing
	 * installs from. An operator tightening a policy would have
	 * unpublished the store.
	 */
	{
		char bad[64];

		publish_fixture(root, "zstd-1.5.7-3-x86_64.cixpkg.minisig", "sig");
		publish_fixture(root, "zstd-1.5.7-3-x86_64.cixpkg", "cixpkg bytes");
		CHECK(store_set_signature_policy(".iso,.cixpkg", bad, sizeof(bad)) == 0,
		      "a policy covering packages applies");

		jw_init(&w);
		manifest_write_json(&w);
		parsed = json_parse(w.buf, w.len);
		CHECK(parsed != NULL, "manifest still generates under that policy");
		if (parsed != NULL) {
			sec = json_object_get(parsed, "packages");
			CHECK(sec != NULL && json_object_get(sec, "zstd-1.5.7-3-x86_64") != NULL,
			      "a .cixpkg requiring a signature is STILL a package");
			sec = json_object_get(parsed, "installers");
			CHECK(sec != NULL && json_object_get(sec, "zstd-1.5.7-3-x86_64") == NULL,
			      "and has not become an installer");
			json_free(parsed);
		}
		jw_free(&w);
		store_set_signature_policy(".iso", bad, sizeof(bad));
	}
	if (g_failures == 0)
		printf("test_manifest: ok\n");
	else
		printf("test_manifest: %d failure(s)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
