/*
 * The name grammar, which parses attacker-supplied path text BEFORE
 * anything has authenticated the request. store_name_is_valid() is the
 * whitelist that makes traversal structurally impossible, so it is the
 * single function in this daemon whose failure is worst.
 *
 * Crashes are the cheap finding here. The expensive ones are property
 * violations, so this target asserts the invariants the rest of the
 * store is written against:
 *
 *   1. canonicalising a name the store accepts must yield a name the
 *      store still accepts. A canonicaliser that emits an invalid name
 *      would rename a published entry into something no request can
 *      ever resolve again.
 *
 *   2. canonical form is a fixed point. store_canonicalize() walks the
 *      store renaming entries; if canonical(canonical(x)) != canonical(x)
 *      that pass never settles, and two runs disagree about what an
 *      artifact is called.
 *
 *   3. a reported suffix really is a suffix. Every caller that wants a
 *      stem computes it as strlen(name) - strlen(suffix).
 *
 * Buffers are heap-allocated at exactly the size the API is told, so
 * ASan's redzones sit immediately past the end; a stack array would let
 * a one-byte overrun land in padding and go unseen.
 */
#include "store.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static char *dup_input(const uint8_t *data, size_t size)
{
	char *s = malloc(size + 1);

	if (s == NULL)
		abort();
	memcpy(s, data, size);
	s[size] = '\0';
	return s;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	char *name;
	char *canon;
	char *again;
	const char *suffix;
	int valid;
	int rc;

	if (size > STORE_NAME_MAX * 2)
		return 0;
	name = dup_input(data, size);

	valid = store_name_is_valid(name);
	(void)store_digest_is_valid(name);

	suffix = store_suffix_of(name);
	if (suffix != NULL) {
		size_t nl = strlen(name);
		size_t sl = strlen(suffix);

		assert(sl <= nl);
		assert(memcmp(name + nl - sl, suffix, sl) == 0);
	}

	/*
	 * The classification trio. store_is_signature() is a fixed fact
	 * about the suffix; store_is_installer() is the tier; and
	 * needs_signature() is operator policy. They were one function
	 * until #15 and the coupling shipped a MANIFEST regression, so
	 * they are each driven here.
	 */
	(void)store_is_signature(name);
	(void)store_is_installer(name);
	(void)store_needs_signature(name);

	canon = malloc(STORE_NAME_MAX);
	if (canon == NULL)
		abort();
	rc = store_canonical_name(name, canon, STORE_NAME_MAX);
	if (rc >= 0) {
		/* Property 1: canonicalising cannot produce a name we reject. */
		assert(valid);
		assert(store_name_is_valid(canon));

		/* Property 2: idempotence. */
		again = malloc(STORE_NAME_MAX);
		if (again == NULL)
			abort();
		assert(store_canonical_name(canon, again, STORE_NAME_MAX) == 0);
		free(again);

		if (rc == 0)
			assert(strcmp(canon, name) == 0);
	} else {
		/*
		 * -1 is "invalid, or would not fit". The second half is
		 * real and deliberate: canonical form is produced into a
		 * STORE_NAME_MAX buffer and can add two characters, so the
		 * top two lengths are accepted by the whitelist and
		 * canonicalise to nothing.
		 *
		 * That is the weaker of the two possible contracts, and it
		 * is the right one -- narrowing the whitelist to make
		 * "valid" total breaks the closure asserted above, because
		 * it narrows the input without narrowing the output. This
		 * assertion is what proved that, by failing on a
		 * 252-character name that canonicalises to 254.
		 */
		assert(!valid || strlen(name) + STORE_CANONICAL_GROWTH >= STORE_NAME_MAX);
	}

	/*
	 * Display splitting is best-effort by contract -- it may guess
	 * the name/version boundary wrong -- so there is nothing to
	 * assert about WHAT it produces. What it must never do is write
	 * past the sizes it was handed, and these buffers are sized to
	 * make that immediately fatal.
	 */
	{
		char *dn = malloc(STORE_NAME_MAX);
		char *dv = malloc(64);
		char *da = malloc(32);
		int release = -1;

		if (dn == NULL || dv == NULL || da == NULL)
			abort();
		store_split_display(name, dn, STORE_NAME_MAX, dv, 64, &release, da, 32);
		assert(strlen(dn) < STORE_NAME_MAX);
		assert(strlen(dv) < 64);
		assert(strlen(da) < 32);
		/* out_release may be NULL -- the contract says so, so check it. */
		store_split_display(name, dn, STORE_NAME_MAX, dv, 64, NULL, da, 32);

		/*
		 * A tiny out_name is the interesting case: truncation must
		 * still terminate.
		 */
		store_split_display(name, dn, 4, dv, 3, &release, da, 2);
		assert(strlen(dn) < 4);
		assert(strlen(dv) < 3);
		assert(strlen(da) < 2);
		free(dn);
		free(dv);
		free(da);
	}

	/*
	 * Version comparison, split on the first NUL-free midpoint so one
	 * input drives both sides. It orders the listing; a comparator
	 * that reads past its arguments does it on names from the wire.
	 */
	{
		size_t half = size / 2;
		char *a = dup_input(data, half);
		char *b = dup_input(data + half, size - half);
		int ab = store_version_cmp(a, b);
		int ba = store_version_cmp(b, a);

		/* Antisymmetry: a sort with an inconsistent comparator is UB. */
		assert((ab == 0 && ba == 0) || (ab < 0 && ba > 0) || (ab > 0 && ba < 0));
		assert(store_version_cmp(a, a) == 0);
		free(a);
		free(b);
	}

	free(canon);
	free(name);
	return 0;
}
