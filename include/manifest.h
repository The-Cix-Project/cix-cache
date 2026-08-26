#ifndef MANIFEST_H
#define MANIFEST_H

#include "json.h"

/*
 * MANIFEST.json, generated live from the store on every request.
 *
 * The file committed to this repository was written by hand alongside
 * the export and is kept as the historical record the importer
 * cross-checks against. It is not what gets served: a checked-in
 * manifest is a second source of truth that starts drifting from the
 * tree the first time anything is published, and a manifest that
 * disagrees with the bytes is worse than none.
 *
 * It carries no authority in either form. The sha256 that approves an
 * artifact lives in the recipe, in git. This is a convenience for
 * humans and for building recipes -- DESIGN.md section 7 says so
 * explicitly, and serving it from the same host as the payload is
 * exactly why it cannot be trusted.
 */
void manifest_write_json(struct json_writer *w);

#endif /* MANIFEST_H */
