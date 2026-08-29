#include "manifest.h"

#include "store.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int emit_entry(const char *name, const char *digest, off_t size, time_t mtime, void *ctx)
{
	struct json_writer *w = ctx;

	/*
	 * Deliberately not in MANIFEST.json: recipe authors read this file
	 * for a checksum and a size, and a timestamp that changes when a
	 * name is republished would make otherwise identical manifests
	 * differ. It is in the API instead, where it is for operators.
	 */
	(void)mtime;
	char file[STORE_NAME_MAX + 16];
	char key[STORE_NAME_MAX];

	snprintf(file, sizeof(file), "%s/%s", STORE_DIR, name);
	/* Keyed by name-version, without the .tar.gz suffix. */
	snprintf(key, sizeof(key), "%.*s", (int)(strlen(name) - 7), name);
	jw_key(w, key);
	jw_obj_open(w);
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
	/*
	 * The "packages" wrapper stays even though it is now the only
	 * section, so anything already reading .packages keeps working.
	 * The "images" key is simply gone -- see ADR-0006.
	 */
	jw_obj_open(w);
	jw_key(w, "packages");
	jw_obj_open(w);
	/*
	 * By name, so that two servers holding the same artifacts emit
	 * byte-identical manifests. Leaving mtime out (above) was half of
	 * that; readdir order defeated the other half.
	 */
	{
		struct store_entry *ents = NULL;
		int n = store_list(&ents);
		int i;

		if (n > 0) {
			qsort(ents, (size_t)n, sizeof(*ents), store_cmp_name);
			for (i = 0; i < n; i++)
				emit_entry(ents[i].name, ents[i].digest, ents[i].size, ents[i].mtime, w);
		}
		free(ents);
	}
	jw_obj_close(w);
	jw_obj_close(w);
}
