#ifndef IMPORTER_H
#define IMPORTER_H

/*
 * One-shot migration of a hand-built static export into the
 * content-addressed store: hash each tarball, move it into blobs/,
 * leave a symlink behind under its published name.
 *
 * Idempotent -- an entry that is already a symlink into blobs/ is
 * counted and skipped, so a re-run after an interruption resumes.
 * Everything is on one filesystem, so each move is a rename() and the
 * 4.6 GB is never copied.
 *
 * Where the checked-in MANIFEST.json records a digest for a file, the
 * computed digest is compared against it and a disagreement is
 * reported rather than quietly accepted: this is the one moment the
 * export's own record can still be checked against its bytes.
 */

struct import_stats {
	int imported;
	int already;
	int mismatched;
	int failed;
	long long bytes;
};

/*
 * Walks <root>/packages and <root>/images. With dry_run non-zero
 * nothing is moved or unlinked and every action is printed instead.
 * Progress goes to stdout, one line per artifact. Returns 0 when
 * nothing failed and nothing mismatched.
 */
int importer_run(const char *root, const char *manifest_path, int dry_run,
                 struct import_stats *out);

#endif /* IMPORTER_H */
