#include "manifest.h"

#include "store.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/*
 * Deliberately not in MANIFEST.json: mtime. Recipe authors read this
 * file for a checksum and a size, and a timestamp that changes when a
 * name is republished would make otherwise identical manifests differ.
 * It is in the API instead, where it is for operators.
 */
static void emit_key(struct json_writer *w, const char *name)
{
	char key[STORE_NAME_MAX];

	/*
	 * Keyed by the identity, never by the file. Two encodings of one
	 * artifact share a stem, so this key is written once per identity
	 * and the encodings live in its "formats" array -- writing it once
	 * per FILE is what emitted the same key twice (#22).
	 */
	store_stem_of(name, key, sizeof(key));
	jw_key(w, key);
}

static void emit_body(struct json_writer *w, const char *name, const char *digest, off_t size)
{
	char file[STORE_NAME_MAX + 16];

	snprintf(file, sizeof(file), "%s/%s", STORE_DIR, name);
	jw_key(w, "file");
	jw_str(w, file);
	jw_key(w, "sha256");
	jw_str(w, digest);
	jw_key(w, "bytes");
	jw_int(w, (long long)size);
}

/*
 * A signature is nested inside the format element it signs rather than
 * given a section of its own, for the same reason it is a column and
 * not a row in the listing: it is part of one artifact, not a second
 * one. Nested, a reader gets both files and both digests without
 * having to know the naming rule that relates them.
 *
 * Inside the ELEMENT and not the identity, because each encoding is
 * signed separately -- a .cixpkg and a .tar.gz of one identity have
 * different bytes and therefore different signatures.
 *
 * The signature's own sha256 is here for completeness -- a mirror needs
 * it -- and not as a way of checking the signature. Both come from this
 * server, so one cannot vouch for the other. What approves a signature
 * is the pinned public key, which does not come from here at all.
 */
static void emit_signature(struct json_writer *w, const char *name)
{
	char sig[STORE_NAME_MAX];
	char digest[STORE_SHA256_MAX];
	off_t size = 0;

	if (store_signature_name(name, sig, sizeof(sig)) != 0)
		return;
	if (store_resolve(sig, digest, sizeof(digest)) != STORE_OK)
		return; /* absent: say nothing rather than assert a file that is not there */
	store_blob_exists(digest, &size);
	jw_key(w, "signature");
	jw_obj_open(w);
	emit_body(w, sig, digest, size);
	jw_obj_close(w);
}

/*
 * One element of an identity's "formats" array: which encoding, where
 * its bytes are, and the signature over those bytes if one exists.
 */
static void emit_format(struct json_writer *w, const struct store_entry *e)
{
	const char *ext = store_suffix_of(e->name);

	jw_obj_open(w);
	jw_key(w, "format");
	jw_str(w, ext != NULL ? ext : "");
	emit_body(w, e->name, e->digest, e->size);
	emit_signature(w, e->name);
	jw_obj_close(w);
}

/*
 * Groups by stem rather than trusting name order to keep an identity's
 * encodings adjacent. Sorted by name they very nearly are, but "nearly"
 * is not a grouping rule: a stem that is another stem plus a dotted
 * component can sort between the two encodings of the shorter one, and
 * the identity would then be emitted twice -- which is the bug this
 * whole change exists to remove, reintroduced by a subtler route.
 */
static int cmp_stem_then_name(const void *a, const void *b)
{
	const struct store_entry *x = *(struct store_entry *const *)a;
	const struct store_entry *y = *(struct store_entry *const *)b;
	char xs[STORE_NAME_MAX];
	char ys[STORE_NAME_MAX];
	int r;

	store_stem_of(x->name, xs, sizeof(xs));
	store_stem_of(y->name, ys, sizeof(ys));
	r = strcmp(xs, ys);
	if (r != 0)
		return r;
	return strcmp(x->name, y->name);
}

/*
 * Writes one tier as identity -> { formats: [...] }.
 *
 * installer_tier selects which tier's artifacts are written, and is
 * the TIER and never the signature policy. These were one function
 * until #15: with the policy configurable, requiring a signature for
 * .cixpkg would have moved every .cixpkg out of "packages" and into
 * "installers", so a config key would have quietly unpublished the
 * store for anything reading packages.
 *
 * Signatures are never rows in either tier. store_is_installer()
 * answers which tier a name's bytes belong to, and an ISO's signature
 * belongs to the installer tier -- that is what puts its bytes in
 * installer_bytes -- but it appears here only as the nested
 * "signature" of the format it signs.
 */
static void write_section(struct json_writer *w, struct store_entry *ents, int n,
                          int installer_tier)
{
	struct store_entry **rows;
	int count = 0;
	int i;

	jw_obj_open(w);
	rows = n > 0 ? calloc((size_t)n, sizeof(*rows)) : NULL;
	if (rows == NULL) {
		jw_obj_close(w);
		return;
	}
	for (i = 0; i < n; i++) {
		if (store_is_signature(ents[i].name))
			continue;
		if (!store_is_installer(ents[i].name) != !installer_tier)
			continue;
		rows[count++] = &ents[i];
	}
	if (count > 0)
		qsort(rows, (size_t)count, sizeof(*rows), cmp_stem_then_name);

	i = 0;
	while (i < count) {
		char stem[STORE_NAME_MAX];
		char other[STORE_NAME_MAX];
		int j = i + 1;

		store_stem_of(rows[i]->name, stem, sizeof(stem));
		while (j < count) {
			store_stem_of(rows[j]->name, other, sizeof(other));
			if (strcmp(stem, other) != 0)
				break;
			j++;
		}
		emit_key(w, rows[i]->name);
		jw_obj_open(w);
		jw_key(w, "formats");
		jw_arr_open(w);
		for (; i < j; i++)
			emit_format(w, rows[i]);
		jw_arr_close(w);
		jw_obj_close(w);
	}
	free(rows);
	jw_obj_close(w);
}

void manifest_write_json(struct json_writer *w)
{
	struct store_entry *ents = NULL;
	int n = store_list(&ents);

	/*
	 * By name, so that two servers holding the same artifacts emit
	 * byte-identical manifests. Leaving mtime out was half of that;
	 * readdir order defeated the other half. write_section() then
	 * orders by stem, which for every name this store has actually
	 * held is the same order -- but not for every name it ACCEPTS,
	 * which is why the grouping does not rely on it. See
	 * cmp_stem_then_name().
	 */
	if (n > 0)
		qsort(ents, (size_t)n, sizeof(*ents), store_cmp_name);

	jw_obj_open(w);

	/*
	 * The "packages" wrapper predates there being anything else, and
	 * stays so that whatever already reads .packages keeps working.
	 * The "images" key is simply gone -- see ADR-0006.
	 */
	jw_key(w, "packages");
	write_section(w, ents, n, 0);

	/*
	 * Installers are their own section and not mixed into packages.
	 * An ISO is never resolved by name@version and has no recipe
	 * behind it. Mixed in, every consumer of packages would grow a
	 * filter, and the first one to forget it would try to install an
	 * ISO.
	 */
	jw_key(w, "installers");
	write_section(w, ents, n, 1);

	jw_obj_close(w);
	free(ents);
}
