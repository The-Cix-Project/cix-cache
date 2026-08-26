#include "manifest.h"

#include "store.h"

#include <stdio.h>
#include <string.h>

/*
 * Splits an image artifact filename into its image name and version.
 * The shape is guaranteed by store_name_is_valid(): the last 64
 * characters before .tar.gz are the manifest hash, and the character
 * before them is the separating hyphen.
 */
static void image_split(const char *name, char *out_name, size_t out_name_size, char *out_version,
                        size_t out_version_size)
{
	size_t stem = strlen(name) - 7;
	size_t base = stem - STORE_SHA256_HEX_LEN - 1;

	snprintf(out_name, out_name_size, "%.*s", (int)base, name);
	snprintf(out_version, out_version_size, "%.*s", STORE_SHA256_HEX_LEN, name + base + 1);
}

static int emit_entry(enum store_tier tier, const char *name, const char *digest, off_t size,
                      void *ctx)
{
	struct json_writer *w = ctx;
	char file[STORE_NAME_MAX + 16];

	snprintf(file, sizeof(file), "%s/%s", store_tier_dir(tier), name);
	if (tier == STORE_TIER_IMAGE) {
		char image_name[STORE_NAME_MAX];
		char version[STORE_SHA256_MAX];

		image_split(name, image_name, sizeof(image_name), version, sizeof(version));
		jw_key(w, image_name);
		jw_obj_open(w);
		jw_key(w, "version");
		jw_str(w, version);
	} else {
		char key[STORE_NAME_MAX];

		snprintf(key, sizeof(key), "%.*s", (int)(strlen(name) - 7), name);
		jw_key(w, key);
		jw_obj_open(w);
	}
	jw_key(w, "file");
	jw_str(w, file);
	jw_key(w, "sha256");
	jw_str(w, digest);
	jw_key(w, "bytes");
	jw_int(w, (long long)size);
	jw_obj_close(w);
	return 0;
}

void manifest_write_json(struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "images");
	jw_obj_open(w);
	store_walk(STORE_TIER_IMAGE, emit_entry, w);
	jw_obj_close(w);
	jw_key(w, "packages");
	jw_obj_open(w);
	store_walk(STORE_TIER_PACKAGE, emit_entry, w);
	jw_obj_close(w);
	jw_obj_close(w);
}
