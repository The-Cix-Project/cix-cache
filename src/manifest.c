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
	const char *ext = store_suffix_of(name);
	char key[STORE_NAME_MAX];

	/* Keyed by the name without its suffix, whatever that suffix is. */
	snprintf(key, sizeof(key), "%.*s", (int)(strlen(name) - (ext != NULL ? strlen(ext) : 0)),
	         name);
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
 * An installer's signature is nested inside its entry rather than given
 * a section of its own, for the same reason it is a column and not a
 * row in the listing: it is part of one artifact, not a second one.
 * Nested, a reader gets both files and both digests without having to
 * know the naming rule that relates them.
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

void manifest_write_json(struct json_writer *w)
{
	struct store_entry *ents = NULL;
	int n = store_list(&ents);
	int i;

	/*
	 * By name, so that two servers holding the same artifacts emit
	 * byte-identical manifests. Leaving mtime out was half of that;
	 * readdir order defeated the other half.
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
	jw_obj_open(w);
	for (i = 0; i < n; i++) {
		if (store_is_signature(ents[i].name) || store_is_installer(ents[i].name))
			continue;
		emit_key(w, ents[i].name);
		jw_obj_open(w);
		emit_body(w, ents[i].name, ents[i].digest, ents[i].size);
		jw_obj_close(w);
	}
	jw_obj_close(w);

	/*
	 * Installers are their own section and not mixed into packages.
	 * "packages" is consumed by a daemon resolving name@version for
	 * install; an ISO is never installed that way and has no recipe
	 * behind it. Mixed in, every consumer of packages would grow a
	 * filter, and the first one to forget it would try to install an
	 * ISO.
	 */
	jw_key(w, "installers");
	jw_obj_open(w);
	for (i = 0; i < n; i++) {
		/*
		 * The TIER, not the signature policy. These were one function
		 * until #15: with the policy configurable, requiring a
		 * signature for .cixpkg would have moved every .cixpkg out of
		 * "packages" and into "installers", and the daemon that
		 * resolves name@version for install would have stopped seeing
		 * it -- a config key quietly unpublishing the store.
		 */
		if (!store_is_installer(ents[i].name))
			continue;
		emit_key(w, ents[i].name);
		jw_obj_open(w);
		emit_body(w, ents[i].name, ents[i].digest, ents[i].size);
		emit_signature(w, ents[i].name);
		jw_obj_close(w);
	}
	jw_obj_close(w);

	jw_obj_close(w);
	free(ents);
}
