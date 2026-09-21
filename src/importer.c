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
 * The digest an entry records for one exact "<tier>/<name>" path, or
 * NULL if this entry is about some other file.
 */
static const char *entry_digest_for(const struct json_value *entry, const char *file)
{
	const char *entry_file;

	if (entry == NULL || entry->type != JSON_OBJECT)
		return NULL;
	entry_file = json_as_string(json_object_get(entry, "file"));
	if (entry_file != NULL && strcmp(entry_file, file) == 0)
		return json_as_string(json_object_get(entry, "sha256"));
	return NULL;
}

/*
 * Looks up the digest a MANIFEST.json recorded for a given
 * "<tier>/<name>" path, or NULL. Matched on the "file" field rather
 * than on the key: an entry is keyed by identity, and one identity may
 * name more than one file.
 *
 * TWO shapes are accepted on purpose, and neither is legacy cruft.
 *
 * An export's checked-in manifest (ADR-0001 -- the historical record
 * this cross-checks bytes against) records one flat entry per
 * identity: { "file", "sha256", "bytes" }. Those files are historical
 * and cannot be reissued, so that shape has to be readable forever.
 *
 * A manifest this server GENERATES records { "formats": [ ... ] }, one
 * element per encoding, because an identity can hold both a .cixpkg
 * and a .tar.gz (#22). Nobody is stopped from saving a served manifest
 * beside an export and importing that.
 *
 * Reading only the flat shape does not fail loudly on such a file, it
 * fails SILENTLY: no entry matches, so every artifact looks like one
 * the manifest never mentioned, and an unmentioned artifact is
 * imported without its bytes being checked against anything. Measured
 * by removing this branch -- "2 imported, 0 mismatched" on a fixture
 * whose recorded digests are both deliberately wrong. The manifest's
 * one job on this path is to be the last place an export's own record
 * can be checked against its bytes, so not finding it is the failure
 * that matters, not a mismatch count.
 */
static const char *section_digest_for(const struct json_value *sec, const char *file)
{
	size_t i;

	if (sec == NULL || sec->type != JSON_OBJECT)
		return NULL;
	for (i = 0; i < sec->u.object.count; i++) {
		const struct json_value *entry = sec->u.object.values[i];
		const struct json_value *formats;
		const char *digest;
		size_t f;

		if (entry == NULL || entry->type != JSON_OBJECT)
			continue;
		formats = json_object_get(entry, "formats");
		if (formats != NULL && formats->type == JSON_ARRAY) {
			for (f = 0; f < formats->u.array.count; f++) {
				digest = entry_digest_for(formats->u.array.items[f], file);
				if (digest != NULL)
					return digest;
			}
			continue;
		}
		digest = entry_digest_for(entry, file);
		if (digest != NULL)
			return digest;
	}
	return NULL;
}

/*
 * BOTH sections are searched, not just "packages".
 *
 * An installer's bytes are in the same tree and its entry is in the
 * same file, but while only "packages" was searched an imported ISO
 * matched nothing, and an artifact the manifest does not mention is
 * imported without being checked against anything. Corrupting an ISO
 * and importing it succeeded silently -- measured, not reasoned.
 *
 * That is the one thing an ISO can least afford. It is verified by a
 * person running minisign against a pinned key, and this import path
 * is upstream of that: bytes that enter the store here are what a
 * later signature would be checked against. ADR-0010 refuses an
 * unsigned bootable at publish for the same reason.
 */
static const char *manifest_digest_for(const struct json_value *root, const char *file)
{
	const char *digest;

	if (root == NULL)
		return NULL;
	digest = section_digest_for(json_object_get(root, "packages"), file);
	if (digest != NULL)
		return digest;
	return section_digest_for(json_object_get(root, "installers"), file);
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

static int import_dir(const char *root, const struct json_value *manifest, int dry_run,
                     struct import_stats *st)
{
	char dir_path[PATH_MAX];
	struct dirent *de;
	DIR *d;

	if ((size_t)snprintf(dir_path, sizeof(dir_path), "%s/%s", root, STORE_DIR) >= sizeof(dir_path))
		return -1;
	d = opendir(dir_path);
	if (d == NULL)
		return 0; /* an empty store is not a failure */
	while ((de = readdir(d)) != NULL) {
		char digest[STORE_SHA256_MAX];
		char path[PATH_MAX];
		char file[PATH_MAX];
		const char *recorded;
		struct stat lst;

		if (de->d_name[0] == '.')
			continue;
		if (!store_name_is_valid(de->d_name)) {
			printf("skip     %s/%s (not a valid artifact name)\n", STORE_DIR, de->d_name);
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
		snprintf(file, sizeof(file), "%s/%s", STORE_DIR, de->d_name);
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
		if (store_publish(de->d_name, digest) != STORE_OK) {
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
	import_dir(root, manifest, dry_run, &st);
	json_free(manifest);
	if (out != NULL)
		*out = st;
	printf("%s: %d imported, %d already, %d mismatched, %d failed, %lld bytes\n",
	       dry_run ? "dry-run" : "import", st.imported, st.already, st.mismatched, st.failed,
	       st.bytes);
	return (st.failed == 0 && st.mismatched == 0) ? 0 : -1;
}
