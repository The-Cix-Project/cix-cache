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

/*
 * Occurrences of a key in the raw JSON.
 *
 * json_object_get() finds the FIRST match, so asking it about a
 * duplicated key answers whichever entry happened to be emitted first
 * and says nothing about the other -- which is exactly how the bug
 * this counts for slipped through a test that looked like it covered
 * it. Two entries sharing a key is the defect; the raw text is the
 * only place that is visible.
 */
static int count_key(const char *json, size_t len, const char *key)
{
	char needle[256];
	size_t nlen;
	size_t i;
	int n = 0;

	nlen = (size_t)snprintf(needle, sizeof(needle), "\"%s\":", key);
	if (nlen >= sizeof(needle) || len < nlen)
		return 0;
	for (i = 0; i + nlen <= len; i++)
		if (memcmp(json + i, needle, nlen) == 0)
			n++;
	return n;
}

/*
 * A string compare that survives a missing field. The shape changed in
 * #22 and the assertions below went from failing to SEGFAULTING,
 * because json_as_string() on an absent key is NULL and strcmp() does
 * not take one. A test that crashes reports nothing at all.
 */
static int streq(const char *a, const char *b)
{
	return a != NULL && b != NULL && strcmp(a, b) == 0;
}

/* The element of an entry's "formats" array for one suffix, or NULL. */
static const struct json_value *format_of(const struct json_value *entry, const char *ext)
{
	const struct json_value *formats;
	size_t i;

	if (entry == NULL || entry->type != JSON_OBJECT)
		return NULL;
	formats = json_object_get(entry, "formats");
	if (formats == NULL || formats->type != JSON_ARRAY)
		return NULL;
	for (i = 0; i < formats->u.array.count; i++) {
		const struct json_value *el = formats->u.array.items[i];

		if (streq(json_as_string(json_object_get(el, "format")), ext))
			return el;
	}
	return NULL;
}

/* How many encodings an entry carries. */
static size_t format_count(const struct json_value *entry)
{
	const struct json_value *formats;

	if (entry == NULL || entry->type != JSON_OBJECT)
		return 0;
	formats = json_object_get(entry, "formats");
	if (formats == NULL || formats->type != JSON_ARRAY)
		return 0;
	return formats->u.array.count;
}

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
	CHECK(format_count(entry) == 1, "one encoding, so one element in formats");
	CHECK(streq(json_as_string(json_object_get(format_of(entry, ".tar.gz"), "file")),
	            "packages/bash-5.2.37-2.tar.gz"),
	      "package file path");
	CHECK(json_as_number(json_object_get(format_of(entry, ".tar.gz"), "bytes")) == 13,
	      "package byte count comes from the blob");
	CHECK(json_object_get(entry, "file") == NULL,
	      "the identity carries no file of its own -- a file belongs to an encoding");

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
	 * .cixpkg out of "packages" -- the section a package is looked up
	 * in by name@version -- and into "installers", which describes
	 * bootables. An operator tightening a policy would have moved
	 * every package into the section nobody looks for one in.
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

	/*
	 * An installer is one entry carrying its signature, not two.
	 *
	 * Caught in production rather than here, which is why this exists:
	 * the installers section filters on store_is_installer(), and that
	 * answers which TIER a name's bytes belong to -- true for an ISO's
	 * signature as well, since its bytes are counted with the ISO.
	 * Without excluding signatures the way the packages section does,
	 * ".iso.minisig" became a row of its own, emit_key() gave it the
	 * same key as the ISO, and the section ended up describing the
	 * signature's 287 bytes as the installer.
	 */
	{
		const struct json_value *inst;

		publish_fixture(root, "cix-installer-2.5.0-1-x86_64.iso.minisig", "sig bytes");
		publish_fixture(root, "cix-installer-2.5.0-1-x86_64.iso", "iso bytes");

		jw_init(&w);
		manifest_write_json(&w);
		parsed = json_parse(w.buf, w.len);
		CHECK(parsed != NULL, "manifest with an installer is valid JSON");
		if (parsed != NULL) {
			sec = json_object_get(parsed, "installers");
			CHECK(sec != NULL && sec->type == JSON_OBJECT, "installers section present");
			inst = sec != NULL ? json_object_get(sec, "cix-installer-2.5.0-1-x86_64") : NULL;
			CHECK(inst != NULL, "the installer is keyed by name-version-release-arch");
			/*
			 * The assertion that actually bites. The ISO and its
			 * signature share a stem, so both emit the same key --
			 * and a lookup would still find the ISO if it came first.
			 */
			CHECK(count_key(w.buf, w.len, "cix-installer-2.5.0-1-x86_64") == 1,
			      "exactly one entry carries that key, not one per file sharing the stem");
			CHECK(format_count(inst) == 1,
			      "one encoding -- the signature is not a second one");
			CHECK(streq(json_as_string(json_object_get(format_of(inst, ".iso"), "file")),
			            "packages/cix-installer-2.5.0-1-x86_64.iso"),
			      "and its file is the ISO, never the signature that shares its stem");
			CHECK(json_as_number(json_object_get(format_of(inst, ".iso"), "bytes")) == 9,
			      "with the ISO's byte count, not the signature's");
			{
				const struct json_value *sig =
				        json_object_get(format_of(inst, ".iso"), "signature");

				CHECK(sig != NULL, "the signature is nested inside the format it signs");
				CHECK(streq(json_as_string(json_object_get(sig, "file")),
				            "packages/cix-installer-2.5.0-1-x86_64.iso.minisig"),
				      "and that is the only place it appears");
			}
			/* Nor is it a row in the other section. */
			sec = json_object_get(parsed, "packages");
			CHECK(sec != NULL && json_object_get(sec, "cix-installer-2.5.0-1-x86_64") == NULL,
			      "an installer is not also a package");
			json_free(parsed);
		}
		jw_free(&w);
	}
	/*
	 * #22. Two encodings of one identity: ONE key, two formats.
	 *
	 * This is the case that did not exist anywhere until a real
	 * CIXPKG was built (#18) -- every store had `artifacts holding
	 * more than one encoding: 0`, so emit_key() writing once per FILE
	 * looked identical to writing once per identity. With both
	 * present it emitted the same key twice, and the file stopped
	 * having one meaning: this repo's json_object_get() takes the
	 * first match and Python and jq take the last, so two readers
	 * disagreed about which encoding and which digest the identity
	 * had.
	 *
	 * count_key() on the raw text is the assertion that bites. A
	 * lookup-based check passes on a duplicated key by finding
	 * whichever came first, which is exactly how this survived a test
	 * that looked like it covered it.
	 */
	{
		const struct json_value *both;

		publish_fixture(root, "coexist-9.9-1-x86_64.tar.gz", "tar.gz bytes");
		publish_fixture(root, "coexist-9.9-1-x86_64.cixpkg", "cixpkg bytes here");

		jw_init(&w);
		manifest_write_json(&w);
		parsed = json_parse(w.buf, w.len);
		CHECK(parsed != NULL, "manifest with two encodings is valid JSON");
		if (parsed != NULL) {
			sec = json_object_get(parsed, "packages");
			both = sec != NULL ? json_object_get(sec, "coexist-9.9-1-x86_64") : NULL;
			CHECK(both != NULL, "the identity is present");
			CHECK(count_key(w.buf, w.len, "coexist-9.9-1-x86_64") == 1,
			      "written ONCE, not once per encoding -- the #22 defect");
			CHECK(format_count(both) == 2, "and carries both encodings");
			CHECK(streq(json_as_string(json_object_get(format_of(both, ".cixpkg"), "file")),
			            "packages/coexist-9.9-1-x86_64.cixpkg"),
			      "the .cixpkg element names the .cixpkg");
			CHECK(streq(json_as_string(json_object_get(format_of(both, ".tar.gz"), "file")),
			            "packages/coexist-9.9-1-x86_64.tar.gz"),
			      "the .tar.gz element names the .tar.gz");
			/*
			 * Distinct digests and sizes per element. Sharing the
			 * identity must not mean sharing the bytes -- conflating
			 * them would reintroduce the ambiguity in a new shape.
			 */
			CHECK(json_as_number(json_object_get(format_of(both, ".tar.gz"), "bytes")) == 12 &&
			              json_as_number(json_object_get(format_of(both, ".cixpkg"), "bytes")) == 17,
			      "each encoding reports its own size");
			CHECK(!streq(json_as_string(json_object_get(format_of(both, ".tar.gz"), "sha256")),
			             json_as_string(json_object_get(format_of(both, ".cixpkg"), "sha256"))),
			      "and its own digest");
			json_free(parsed);
		}
		jw_free(&w);
	}

	if (g_failures == 0)
		printf("test_manifest: ok\n");
	else
		printf("test_manifest: %d failure(s)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
