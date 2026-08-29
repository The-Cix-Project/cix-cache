#include "store.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static char g_root[PATH_MAX];
static unsigned long g_tmp_counter;

const char *store_root(void)
{
	return g_root;
}

const char *store_error_str(enum store_error e)
{
	switch (e) {
	case STORE_OK:
		return "ok";
	case STORE_ERR_INVALID_NAME:
		return "invalid artifact name";
	case STORE_ERR_NOT_FOUND:
		return "not found";
	case STORE_ERR_CONFLICT:
		return "already published with a different digest";
	case STORE_ERR_IO:
		return "store i/o error";
	}
	return "unknown";
}

static int mkdir_if_absent(const char *path)
{
	if (mkdir(path, 0755) == 0)
		return 0;
	if (errno == EEXIST)
		return 0;
	fprintf(stderr, "store: mkdir %s: %s\n", path, strerror(errno));
	return -1;
}

int store_init(const char *root)
{
	static const char *subdirs[] = { "blobs", STORE_DIR, "tmp" };
	char path[PATH_MAX];
	size_t i;

	if (root == NULL || root[0] == '\0') {
		fprintf(stderr, "store: empty root\n");
		return -1;
	}
	if ((size_t)snprintf(g_root, sizeof(g_root), "%s", root) >= sizeof(g_root)) {
		fprintf(stderr, "store: root path too long\n");
		return -1;
	}
	if (mkdir_if_absent(g_root) != 0)
		return -1;
	for (i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); i++) {
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", g_root, subdirs[i]) >= sizeof(path)) {
			fprintf(stderr, "store: path too long under root\n");
			return -1;
		}
		if (mkdir_if_absent(path) != 0)
			return -1;
	}
	return 0;
}

int store_digest_is_valid(const char *s)
{
	size_t i;

	if (s == NULL)
		return 0;
	for (i = 0; i < STORE_SHA256_HEX_LEN; i++) {
		char c = s[i];

		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			return 0;
	}
	return s[STORE_SHA256_HEX_LEN] == '\0';
}

int store_name_is_valid(const char *name)
{
	size_t len;
	size_t i;

	if (name == NULL || name[0] == '\0' || name[0] == '.')
		return 0;
	len = strlen(name);
	if (len >= STORE_NAME_MAX)
		return 0;
	for (i = 0; i < len; i++) {
		char c = name[i];

		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		      c == '.' || c == '_' || c == '-'))
			return 0;
		if (c == '.' && name[i + 1] == '.')
			return 0;
	}
	if (len <= 7 || strcmp(name + len - 7, ".tar.gz") != 0)
		return 0;
	return 1;
}

/*
 * Offset of the first digit of the release inside a stem, or 0 when
 * the stem carries none. See store_canonical_name() for the rule and
 * why it is the tail alone that is parsed.
 */
static size_t release_offset(const char *name, size_t stem)
{
	size_t dash = 0;
	size_t i;
	int version_before = 0;

	for (i = stem; i > 0; i--) {
		if (name[i - 1] == '-') {
			dash = i - 1;
			break;
		}
	}
	/* No hyphen, or nothing on one side of it, means no release. */
	if (dash == 0 || dash + 1 >= stem)
		return 0;

	for (i = dash + 1; i < stem; i++) {
		if (name[i] < '0' || name[i] > '9')
			return 0;
	}

	/*
	 * A release is a revision OF a version, so one has to precede it.
	 * Without this, a date-style version like foo-20250101 would read
	 * as release 20250101 of a package called foo.
	 */
	for (i = dash; i > 0; i--) {
		if (name[i - 1] == '-')
			break;
		if (name[i - 1] >= '0' && name[i - 1] <= '9')
			version_before = 1;
	}
	if (!version_before)
		return 0;
	return dash + 1;
}

/*
 * True if the stem's last hyphen-separated component contains a digit,
 * which is what stands in for "this name carries a version at all".
 */
static int has_version(const char *name, size_t stem)
{
	size_t i;

	for (i = stem; i > 0; i--) {
		if (name[i - 1] == '-')
			break;
		if (name[i - 1] >= '0' && name[i - 1] <= '9')
			return 1;
	}
	return 0;
}

int store_canonical_name(const char *name, char *out, size_t out_size)
{
	size_t len;
	size_t stem;
	size_t rel;
	size_t z;

	if (!store_name_is_valid(name))
		return -1;
	len = strlen(name);
	stem = len - 7; /* store_name_is_valid() guarantees the .tar.gz */
	rel = release_offset(name, stem);

	if (rel == 0) {
		/*
		 * A release qualifies a version, so a name carrying no
		 * version has nothing for it to qualify. Appending -1 to
		 * "noversion" yields "noversion-1", which then reads as
		 * version 1 and canonicalizes again -- so canonical form
		 * would not be a fixed point, and an entry stored under one
		 * spelling would be unreachable under the other. Leave such
		 * a name exactly as it is rather than inventing a version.
		 */
		if (!has_version(name, stem)) {
			if ((size_t)snprintf(out, out_size, "%s", name) >= out_size)
				return -1;
			return 0;
		}
		if ((size_t)snprintf(out, out_size, "%.*s-1.tar.gz", (int)stem, name) >= out_size)
			return -1;
	} else {
		/* Skip leading zeros textually; strtol here could overflow. */
		z = rel;
		while (z + 1 < stem && name[z] == '0')
			z++;
		if ((size_t)snprintf(out, out_size, "%.*s-%.*s.tar.gz", (int)(rel - 1), name,
		                     (int)(stem - z), name + z) >= out_size)
			return -1;
	}
	return strcmp(out, name) != 0 ? 1 : 0;
}

void store_split_display(const char *name, char *out_name, size_t out_name_size, char *out_version,
                         size_t out_version_size, int *out_release)
{
	size_t len = strlen(name);
	size_t stem = len > 7 ? len - 7 : len;
	size_t rel;
	size_t i;

	out_name[0] = '\0';
	out_version[0] = '\0';

	rel = release_offset(name, stem);
	if (out_release != NULL) {
		*out_release = 1;
		if (rel != 0) {
			int v = 0;

			for (i = rel; i < stem; i++)
				v = v * 10 + (name[i] - '0');
			*out_release = v;
		}
	}
	/* The release is reported on its own, so keep it out of the version. */
	if (rel != 0)
		stem = rel - 1;

	for (i = 0; i + 1 < stem; i++) {
		char next = name[i + 1];
		int starts_version = (next >= '0' && next <= '9') ||
		                     (next == 'v' && i + 2 < stem && name[i + 2] >= '0' &&
		                      name[i + 2] <= '9');

		if (name[i] == '-' && starts_version) {
			snprintf(out_name, out_name_size, "%.*s", (int)i, name);
			snprintf(out_version, out_version_size, "%.*s", (int)(stem - i - 1), name + i + 1);
			return;
		}
	}
	/* No boundary found -- say so by leaving the version empty. */
	snprintf(out_name, out_name_size, "%.*s", (int)stem, name);
}

static int raw_entry_path(const char *name, char *out, size_t out_size)
{
	if ((size_t)snprintf(out, out_size, "%s/%s/%s", g_root, STORE_DIR, name) >= out_size)
		return -1;
	return 0;
}

/*
 * Every path into the store is canonicalised here, at the one choke
 * point every caller already goes through. That is what makes a
 * non-canonical request an alias rather than a miss: exactly one file
 * exists per artifact and both spellings of its name reach it, so no
 * second entry, no second checksum, and nothing to drift.
 *
 * Doing it here rather than in each caller is also what keeps resolve,
 * publish and unpublish from ever disagreeing about which file a name
 * means -- a disagreement that would show up as a 409 against a name
 * that appears not to exist.
 */
static int entry_path(const char *name, char *out, size_t out_size)
{
	char canonical[STORE_NAME_MAX];

	if (store_canonical_name(name, canonical, sizeof(canonical)) < 0)
		return -1;
	return raw_entry_path(canonical, out, out_size);
}

static int blob_path(const char *digest, char *out, size_t out_size)
{
	if ((size_t)snprintf(out, out_size, "%s/blobs/%s", g_root, digest) >= out_size)
		return -1;
	return 0;
}

/* Reads the digest out of the symlink at an exact path. */
static enum store_error resolve_path(const char *path, char *out_digest, size_t out_digest_size)
{
	char target[PATH_MAX];
	const char *base;
	ssize_t n;

	if (out_digest_size < STORE_SHA256_MAX)
		return STORE_ERR_IO;
	n = readlink(path, target, sizeof(target) - 1);
	if (n < 0)
		return errno == ENOENT ? STORE_ERR_NOT_FOUND : STORE_ERR_IO;
	target[n] = '\0';
	base = strrchr(target, '/');
	base = base != NULL ? base + 1 : target;
	if (!store_digest_is_valid(base))
		return STORE_ERR_IO;
	memcpy(out_digest, base, STORE_SHA256_MAX - 1);
	out_digest[STORE_SHA256_MAX - 1] = '\0';
	return STORE_OK;
}

enum store_error store_resolve(const char *name, char *out_digest, size_t out_digest_size)
{
	char path[PATH_MAX];

	if (out_digest_size < STORE_SHA256_MAX)
		return STORE_ERR_IO;
	if (!store_name_is_valid(name))
		return STORE_ERR_INVALID_NAME;
	if (entry_path(name, path, sizeof(path)) != 0)
		return STORE_ERR_INVALID_NAME;
	return resolve_path(path, out_digest, out_digest_size);
}

enum store_error store_open(const char *name, int *out_fd, off_t *out_size, char *out_digest,
                            size_t out_digest_size)
{
	char digest[STORE_SHA256_MAX];
	char path[PATH_MAX];
	enum store_error e;
	struct stat st;
	int fd;

	e = store_resolve(name, digest, sizeof(digest));
	if (e != STORE_OK)
		return e;
	if (blob_path(digest, path, sizeof(path)) != 0)
		return STORE_ERR_IO;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return errno == ENOENT ? STORE_ERR_NOT_FOUND : STORE_ERR_IO;
	if (fstat(fd, &st) != 0) {
		close(fd);
		return STORE_ERR_IO;
	}
	*out_fd = fd;
	*out_size = st.st_size;
	if (out_digest != NULL && out_digest_size >= STORE_SHA256_MAX)
		snprintf(out_digest, out_digest_size, "%s", digest);
	return STORE_OK;
}

int store_blob_exists(const char *digest, off_t *out_size)
{
	char path[PATH_MAX];
	struct stat st;

	if (!store_digest_is_valid(digest))
		return 0;
	if (blob_path(digest, path, sizeof(path)) != 0)
		return 0;
	if (stat(path, &st) != 0)
		return 0;
	if (out_size != NULL)
		*out_size = st.st_size;
	return 1;
}

enum store_error store_blob_adopt(const char *tmp_path, const char *digest)
{
	char path[PATH_MAX];

	if (!store_digest_is_valid(digest))
		return STORE_ERR_IO;
	if (blob_path(digest, path, sizeof(path)) != 0)
		return STORE_ERR_IO;
	/*
	 * Already present means some other name already published these
	 * exact bytes. Drop the upload and let both names share the one
	 * copy -- that is the whole reason the store is content-addressed.
	 */
	if (access(path, F_OK) == 0) {
		unlink(tmp_path);
		return STORE_OK;
	}
	if (rename(tmp_path, path) != 0) {
		fprintf(stderr, "store: rename %s -> %s: %s\n", tmp_path, path, strerror(errno));
		return STORE_ERR_IO;
	}
	/* Read-only: a blob is named by its content, so it can never legally change. */
	if (chmod(path, 0444) != 0)
		fprintf(stderr, "store: chmod %s: %s\n", path, strerror(errno));
	return STORE_OK;
}

enum store_error store_publish(const char *name, const char *digest)
{
	char existing[STORE_SHA256_MAX];
	char link_path[PATH_MAX];
	char tmp_link[PATH_MAX];
	char target[PATH_MAX];
	enum store_error e;

	if (!store_name_is_valid(name))
		return STORE_ERR_INVALID_NAME;
	if (!store_digest_is_valid(digest))
		return STORE_ERR_IO;
	if (!store_blob_exists(digest, NULL))
		return STORE_ERR_NOT_FOUND;

	e = store_resolve(name, existing, sizeof(existing));
	if (e == STORE_OK) {
		if (strcmp(existing, digest) == 0)
			return STORE_OK; /* idempotent republish of identical bytes */
		return STORE_ERR_CONFLICT;
	}
	if (e != STORE_ERR_NOT_FOUND)
		return e;

	if (entry_path(name, link_path, sizeof(link_path)) != 0)
		return STORE_ERR_INVALID_NAME;
	/* Relative, so the whole tree can be moved or served from anywhere. */
	if ((size_t)snprintf(target, sizeof(target), "../blobs/%s", digest) >= sizeof(target))
		return STORE_ERR_IO;
	if ((size_t)snprintf(tmp_link, sizeof(tmp_link), "%s/tmp/.pub.%d.%lu", g_root, (int)getpid(),
	                     g_tmp_counter++) >= sizeof(tmp_link))
		return STORE_ERR_IO;
	unlink(tmp_link);
	if (symlink(target, tmp_link) != 0) {
		fprintf(stderr, "store: symlink %s: %s\n", tmp_link, strerror(errno));
		return STORE_ERR_IO;
	}
	/* rename() over the final name, so a reader never sees a half-published entry. */
	if (rename(tmp_link, link_path) != 0) {
		fprintf(stderr, "store: publish %s: %s\n", link_path, strerror(errno));
		unlink(tmp_link);
		return STORE_ERR_IO;
	}
	return STORE_OK;
}

enum store_error store_unpublish(const char *name)
{
	char path[PATH_MAX];

	if (!store_name_is_valid(name))
		return STORE_ERR_INVALID_NAME;
	if (entry_path(name, path, sizeof(path)) != 0)
		return STORE_ERR_INVALID_NAME;
	if (unlink(path) != 0)
		return errno == ENOENT ? STORE_ERR_NOT_FOUND : STORE_ERR_IO;
	return STORE_OK;
}

int store_tmp_path(char *out, size_t out_size)
{
	if ((size_t)snprintf(out, out_size, "%s/tmp/.up.%d.%lu", g_root, (int)getpid(),
	                     g_tmp_counter++) >= out_size)
		return -1;
	return 0;
}

int store_hash_file(const char *path, char *out, size_t out_size)
{
	char buf[128];
	int pipefd[2];
	size_t got = 0;
	pid_t pid;
	int status;

	if (out_size < STORE_SHA256_MAX)
		return -1;
	out[0] = '\0';
	if (pipe(pipefd) != 0)
		return -1;
	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (pid == 0) {
		char *argv[] = { (char *)STORE_SHA256SUM_BIN, (char *)path, NULL };

		close(pipefd[0]);
		if (dup2(pipefd[1], STDOUT_FILENO) < 0)
			_exit(127);
		close(pipefd[1]);
		execve(STORE_SHA256SUM_BIN, argv, environ);
		_exit(127);
	}
	close(pipefd[1]);
	while (got < sizeof(buf) - 1) {
		ssize_t n = read(pipefd[0], buf + got, sizeof(buf) - 1 - got);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		got += (size_t)n;
	}
	close(pipefd[0]);
	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;
	if (got < STORE_SHA256_HEX_LEN)
		return -1;
	memcpy(out, buf, STORE_SHA256_HEX_LEN);
	out[STORE_SHA256_HEX_LEN] = '\0';
	return store_digest_is_valid(out) ? 0 : -1;
}

int store_walk(int (*fn)(const char *name, const char *digest, off_t size, time_t mtime, void *ctx),
               void *ctx)
{
	char dir_path[PATH_MAX];
	struct dirent *de;
	DIR *d;
	int rc = 0;

	if ((size_t)snprintf(dir_path, sizeof(dir_path), "%s/%s", g_root, STORE_DIR) >=
	    sizeof(dir_path))
		return -1;
	d = opendir(dir_path);
	if (d == NULL)
		return -1;
	while ((de = readdir(d)) != NULL) {
		char digest[STORE_SHA256_MAX];
		char link_path[PATH_MAX];
		struct stat lst;
		time_t mtime = 0;
		off_t size = -1;

		if (de->d_name[0] == '.')
			continue;
		if (!store_name_is_valid(de->d_name))
			continue;
		/*
		 * The LITERAL name on disk, never store_resolve(), which
		 * canonicalises: an entry not yet migrated would resolve to
		 * a canonical name that does not exist, be skipped here, and
		 * so vanish from every listing -- and from the live set the
		 * collector builds, which would then free its blob. The walk
		 * reports the store as it is, not as it ought to be spelled.
		 */
		if (raw_entry_path(de->d_name, link_path, sizeof(link_path)) != 0)
			continue;
		if (resolve_path(link_path, digest, sizeof(digest)) != STORE_OK)
			continue;
		store_blob_exists(digest, &size);
		if (lstat(link_path, &lst) == 0)
			mtime = lst.st_mtime;
		rc = fn(de->d_name, digest, size, mtime, ctx);
		if (rc != 0)
			break;
	}
	closedir(d);
	return rc;
}

/*
 * Names needing a rename are collected before any is performed rather
 * than renamed inside the walk: renaming an entry while readdir() is
 * mid-directory can make the reader skip or repeat entries, and a
 * migration that quietly misses one is worse than one that refuses to
 * start.
 */
struct rename_set {
	char *slots;
	size_t count;
	size_t cap;
};

static int collect_noncanonical(const char *name, const char *digest, off_t size, time_t mtime,
                                void *ctx)
{
	struct rename_set *set = ctx;
	char canonical[STORE_NAME_MAX];

	(void)digest;
	(void)size;
	(void)mtime;
	if (store_canonical_name(name, canonical, sizeof(canonical)) != 1)
		return 0; /* already canonical, or not a name we can rewrite */
	if (set->count == set->cap) {
		size_t cap = set->cap != 0 ? set->cap * 2 : 64;
		char *grown = realloc(set->slots, cap * STORE_NAME_MAX);

		if (grown == NULL)
			return -1;
		set->slots = grown;
		set->cap = cap;
	}
	snprintf(set->slots + set->count * STORE_NAME_MAX, STORE_NAME_MAX, "%s", name);
	set->count++;
	return 0;
}

int store_canonicalize(int dry_run, int *out_renamed, int *out_conflicts)
{
	struct rename_set set;
	char canonical[STORE_NAME_MAX];
	char from[PATH_MAX];
	char to[PATH_MAX];
	struct stat st;
	size_t i;
	int renamed = 0;
	int conflicts = 0;
	int rc = 0;

	set.slots = NULL;
	set.count = 0;
	set.cap = 0;
	if (store_walk(collect_noncanonical, &set) != 0) {
		free(set.slots);
		fprintf(stderr, "cixcached: cannot read the store to canonicalize it\n");
		return -1;
	}

	for (i = 0; i < set.count; i++) {
		const char *name = set.slots + i * STORE_NAME_MAX;

		if (store_canonical_name(name, canonical, sizeof(canonical)) != 1)
			continue;
		/*
		 * Raw paths on both sides: entry_path() canonicalises, so it
		 * would hand back the same path for the old name and the new
		 * one and the rename would be a no-op onto itself.
		 */
		if (raw_entry_path(name, from, sizeof(from)) != 0 ||
		    raw_entry_path(canonical, to, sizeof(to)) != 0) {
			fprintf(stderr, "canonicalize: %s: path too long\n", name);
			rc = -1;
			continue;
		}
		if (lstat(to, &st) == 0) {
			/*
			 * Both names already exist. Renaming would destroy one of
			 * them, and which one is a question about two different
			 * byte sequences that only an operator can answer.
			 */
			fprintf(stderr, "canonicalize: %s -> %s: target exists, skipped\n", name,
			        canonical);
			conflicts++;
			continue;
		}
		if (dry_run) {
			printf("would rename %s -> %s\n", name, canonical);
			renamed++;
			continue;
		}
		if (rename(from, to) != 0) {
			fprintf(stderr, "canonicalize: %s -> %s: %s\n", name, canonical, strerror(errno));
			rc = -1;
			continue;
		}
		printf("renamed %s -> %s\n", name, canonical);
		renamed++;
	}

	free(set.slots);
	if (out_renamed != NULL)
		*out_renamed = renamed;
	if (out_conflicts != NULL)
		*out_conflicts = conflicts;
	return rc;
}

struct entry_set {
	struct store_entry *v;
	size_t count;
	size_t cap;
};

static int collect_entry(const char *name, const char *digest, off_t size, time_t mtime, void *ctx)
{
	struct entry_set *set = ctx;
	struct store_entry *e;

	if (set->count == set->cap) {
		size_t cap = set->cap != 0 ? set->cap * 2 : 64;
		struct store_entry *grown = realloc(set->v, cap * sizeof(*grown));

		if (grown == NULL)
			return -1;
		set->v = grown;
		set->cap = cap;
	}
	e = &set->v[set->count];
	snprintf(e->name, sizeof(e->name), "%s", name);
	snprintf(e->digest, sizeof(e->digest), "%s", digest);
	e->size = size;
	e->mtime = mtime;
	set->count++;
	return 0;
}

int store_list(struct store_entry **out)
{
	struct entry_set set;

	set.v = NULL;
	set.count = 0;
	set.cap = 0;
	if (store_walk(collect_entry, &set) != 0) {
		free(set.v);
		return -1;
	}
	*out = set.v;
	return (int)set.count;
}

int store_cmp_newest(const void *a, const void *b)
{
	const struct store_entry *x = a;
	const struct store_entry *y = b;

	if (x->mtime != y->mtime)
		return x->mtime < y->mtime ? 1 : -1;
	/* Names are unique, so this makes the order total and stable. */
	return strcmp(x->name, y->name);
}

int store_cmp_name(const void *a, const void *b)
{
	return strcmp(((const struct store_entry *)a)->name, ((const struct store_entry *)b)->name);
}

static int digest_cmp(const void *a, const void *b)
{
	return strcmp((const char *)a, (const char *)b);
}

struct live_set {
	char *slots;
	size_t count;
	size_t cap;
};

static int collect_live(const char *name, const char *digest, off_t size, time_t mtime, void *ctx)
{
	struct live_set *set = ctx;

	(void)name;
	(void)size;
	(void)mtime;
	if (set->count == set->cap) {
		size_t cap = set->cap != 0 ? set->cap * 2 : 64;
		char *grown = realloc(set->slots, cap * STORE_SHA256_MAX);

		if (grown == NULL)
			return -1;
		set->slots = grown;
		set->cap = cap;
	}
	memcpy(set->slots + set->count * STORE_SHA256_MAX, digest, STORE_SHA256_MAX);
	set->count++;
	return 0;
}

int store_gc(int dry_run, long long *out_bytes_freed)
{
	char dir_path[PATH_MAX];
	struct live_set set;
	struct dirent *de;
	long long freed = 0;
	DIR *d;
	int removed = 0;

	memset(&set, 0, sizeof(set));
	if (out_bytes_freed != NULL)
		*out_bytes_freed = 0;
	/*
	 * A symlink target carries the digest but a blob's inode carries
	 * no reverse pointer, so the live set has to be built by reading
	 * every published entry first. That is the cost of the O(1)
	 * digest lookup on the serving path, paid here where it is rare.
	 */
	if (store_walk(collect_live, &set) != 0) {
		free(set.slots);
		return -1;
	}
	if (set.count > 1)
		qsort(set.slots, set.count, STORE_SHA256_MAX, digest_cmp);

	if ((size_t)snprintf(dir_path, sizeof(dir_path), "%s/blobs", g_root) >= sizeof(dir_path)) {
		free(set.slots);
		return -1;
	}
	d = opendir(dir_path);
	if (d == NULL) {
		free(set.slots);
		return -1;
	}
	while ((de = readdir(d)) != NULL) {
		char path[PATH_MAX];
		struct stat st;

		if (!store_digest_is_valid(de->d_name))
			continue;
		if (set.count != 0 &&
		    bsearch(de->d_name, set.slots, set.count, STORE_SHA256_MAX, digest_cmp) != NULL)
			continue;
		if (blob_path(de->d_name, path, sizeof(path)) != 0)
			continue;
		if (stat(path, &st) == 0)
			freed += (long long)st.st_size;
		if (!dry_run && unlink(path) != 0) {
			fprintf(stderr, "store: gc unlink %s: %s\n", path, strerror(errno));
			continue;
		}
		removed++;
	}
	closedir(d);
	free(set.slots);
	if (out_bytes_freed != NULL)
		*out_bytes_freed = freed;
	return removed;
}
