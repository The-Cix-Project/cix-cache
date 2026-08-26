#include "importer.h"

#include "json.h"
#include "store.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Looks up the digest the checked-in MANIFEST.json recorded for a
 * given "<tier>/<name>" path, or NULL. The manifest keys entries by
 * artifact name rather than by path, so the "file" field is what has
 * to be matched.
 */
static const char *manifest_digest_for(const struct json_value *root, const char *file)
{
	static const char *sections[] = { "images", "packages" };
	size_t s;

	if (root == NULL)
		return NULL;
	for (s = 0; s < sizeof(sections) / sizeof(sections[0]); s++) {
		const struct json_value *sec = json_object_get(root, sections[s]);
		size_t i;

		if (sec == NULL || sec->type != JSON_OBJECT)
			continue;
		for (i = 0; i < sec->u.object.count; i++) {
			const struct json_value *entry = sec->u.object.values[i];
			const char *entry_file;

			if (entry == NULL || entry->type != JSON_OBJECT)
				continue;
			entry_file = json_as_string(json_object_get(entry, "file"));
			if (entry_file != NULL && strcmp(entry_file, file) == 0)
				return json_as_string(json_object_get(entry, "sha256"));
		}
	}
	return NULL;
}

static struct json_value *load_manifest(const char *path)
{
	struct json_value *v;
	char *buf;
	long size;
	FILE *f;

	if (path == NULL)
		return NULL;
	f = fopen(path, "rb");
	if (f == NULL)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	buf = malloc((size_t)size + 1);
	if (buf == NULL) {
		fclose(f);
		return NULL;
	}
	if (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size) {
		free(buf);
		fclose(f);
		return NULL;
	}
	buf[size] = '\0';
	fclose(f);
	v = json_parse(buf, (size_t)size);
	free(buf);
	return v;
}

static int import_tier(const char *root, enum store_tier tier, const struct json_value *manifest,
                       int dry_run, struct import_stats *st)
{
	char dir_path[PATH_MAX];
	struct dirent *de;
	DIR *d;

	if ((size_t)snprintf(dir_path, sizeof(dir_path), "%s/%s", root, store_tier_dir(tier)) >=
	    sizeof(dir_path))
		return -1;
	d = opendir(dir_path);
	if (d == NULL)
		return 0; /* a tier with nothing in it is not a failure */
	while ((de = readdir(d)) != NULL) {
		char digest[STORE_SHA256_MAX];
		char path[PATH_MAX];
		char file[PATH_MAX];
		const char *recorded;
		struct stat lst;

		if (de->d_name[0] == '.')
			continue;
		if (!store_name_is_valid(tier, de->d_name)) {
			printf("skip     %s/%s (not a valid artifact name)\n", store_tier_dir(tier),
			       de->d_name);
			continue;
		}
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir_path, de->d_name) >= sizeof(path))
			continue;
		if (lstat(path, &lst) != 0)
			continue;
		if (S_ISLNK(lst.st_mode)) {
			st->already++;
			continue; /* already imported -- a re-run resumes rather than redoes */
		}
		if (!S_ISREG(lst.st_mode))
			continue;

		if (store_hash_file(path, digest, sizeof(digest)) != 0) {
			fprintf(stderr, "import: cannot hash %s\n", path);
			st->failed++;
			continue;
		}
		snprintf(file, sizeof(file), "%s/%s", store_tier_dir(tier), de->d_name);
		recorded = manifest_digest_for(manifest, file);
		if (recorded != NULL && strcasecmp(recorded, digest) != 0) {
			/*
			 * The export's own record disagrees with its bytes.
			 * This is the last moment that can be noticed, so say
			 * so loudly and leave the file alone rather than
			 * publish something no recipe will accept.
			 */
			fprintf(stderr, "import: MISMATCH %s: manifest says %s, bytes hash %s\n", file,
			        recorded, digest);
			st->mismatched++;
			continue;
		}
		st->bytes += (long long)lst.st_size;
		if (dry_run) {
			printf("would   %s -> blobs/%s (%lld bytes)\n", file, digest,
			       (long long)lst.st_size);
			st->imported++;
			continue;
		}
		if (store_blob_adopt(path, digest) != STORE_OK) {
			st->failed++;
			continue;
		}
		if (store_publish(tier, de->d_name, digest) != STORE_OK) {
			fprintf(stderr, "import: cannot publish %s\n", file);
			st->failed++;
			continue;
		}
		printf("import  %s -> blobs/%s\n", file, digest);
		st->imported++;
	}
	closedir(d);
	return 0;
}

int importer_run(const char *root, const char *manifest_path, int dry_run,
                 struct import_stats *out)
{
	struct json_value *manifest;
	struct import_stats st;

	memset(&st, 0, sizeof(st));
	manifest = load_manifest(manifest_path);
	if (manifest == NULL && manifest_path != NULL)
		fprintf(stderr, "import: no usable manifest at %s -- proceeding without cross-check\n",
		        manifest_path);
	import_tier(root, STORE_TIER_PACKAGE, manifest, dry_run, &st);
	import_tier(root, STORE_TIER_IMAGE, manifest, dry_run, &st);
	json_free(manifest);
	if (out != NULL)
		*out = st;
	printf("%s: %d imported, %d already, %d mismatched, %d failed, %lld bytes\n",
	       dry_run ? "dry-run" : "import", st.imported, st.already, st.mismatched, st.failed,
	       st.bytes);
	return (st.failed == 0 && st.mismatched == 0) ? 0 : -1;
}
