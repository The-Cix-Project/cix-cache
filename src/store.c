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

const char *store_tier_dir(enum store_tier tier)
{
	return tier == STORE_TIER_IMAGE ? "images" : "packages";
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
	static const char *subdirs[] = { "blobs", "packages", "images", "tmp" };
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

int store_name_is_valid(enum store_tier tier, const char *name)
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
	/*
	 * An image artifact's filename is <name>-<image_version>.tar.gz
	 * where image_version is sha256 of the image's sorted
	 * "name@version,..." manifest -- computed on the host from recipe
	 * text alone. Nothing else can ever be requested, so nothing else
	 * is accepted.
	 */
	if (tier == STORE_TIER_IMAGE) {
		size_t stem = len - 7;

		if (stem < STORE_SHA256_HEX_LEN + 1)
			return 0;
		if (name[stem - STORE_SHA256_HEX_LEN - 1] != '-')
			return 0;
		for (i = stem - STORE_SHA256_HEX_LEN; i < stem; i++) {
			char c = name[i];

			if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
				return 0;
		}
	}
	return 1;
}

static int tier_entry_path(enum store_tier tier, const char *name, char *out, size_t out_size)
{
	if ((size_t)snprintf(out, out_size, "%s/%s/%s", g_root, store_tier_dir(tier), name) >=
	    out_size)
		return -1;
	return 0;
}

static int blob_path(const char *digest, char *out, size_t out_size)
{
	if ((size_t)snprintf(out, out_size, "%s/blobs/%s", g_root, digest) >= out_size)
		return -1;
	return 0;
}

enum store_error store_resolve(enum store_tier tier, const char *name, char *out_digest,
                               size_t out_digest_size)
{
	char path[PATH_MAX];
	char target[PATH_MAX];
	const char *base;
	ssize_t n;

	if (out_digest_size < STORE_SHA256_MAX)
		return STORE_ERR_IO;
	if (!store_name_is_valid(tier, name))
		return STORE_ERR_INVALID_NAME;
	if (tier_entry_path(tier, name, path, sizeof(path)) != 0)
		return STORE_ERR_INVALID_NAME;
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

enum store_error store_open(enum store_tier tier, const char *name, int *out_fd, off_t *out_size,
                            char *out_digest, size_t out_digest_size)
{
	char digest[STORE_SHA256_MAX];
	char path[PATH_MAX];
	enum store_error e;
	struct stat st;
	int fd;

	e = store_resolve(tier, name, digest, sizeof(digest));
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

enum store_error store_publish(enum store_tier tier, const char *name, const char *digest)
{
	char existing[STORE_SHA256_MAX];
	char link_path[PATH_MAX];
	char tmp_link[PATH_MAX];
	char target[PATH_MAX];
	enum store_error e;

	if (!store_name_is_valid(tier, name))
		return STORE_ERR_INVALID_NAME;
	if (!store_digest_is_valid(digest))
		return STORE_ERR_IO;
	if (!store_blob_exists(digest, NULL))
		return STORE_ERR_NOT_FOUND;

	e = store_resolve(tier, name, existing, sizeof(existing));
	if (e == STORE_OK) {
		if (strcmp(existing, digest) == 0)
			return STORE_OK; /* idempotent republish of identical bytes */
		return STORE_ERR_CONFLICT;
	}
	if (e != STORE_ERR_NOT_FOUND)
		return e;

	if (tier_entry_path(tier, name, link_path, sizeof(link_path)) != 0)
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

enum store_error store_unpublish(enum store_tier tier, const char *name)
{
	char path[PATH_MAX];

	if (!store_name_is_valid(tier, name))
		return STORE_ERR_INVALID_NAME;
	if (tier_entry_path(tier, name, path, sizeof(path)) != 0)
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

int store_walk(enum store_tier tier,
               int (*fn)(enum store_tier tier, const char *name, const char *digest, off_t size,
                         void *ctx),
               void *ctx)
{
	char dir_path[PATH_MAX];
	struct dirent *de;
	DIR *d;
	int rc = 0;

	if ((size_t)snprintf(dir_path, sizeof(dir_path), "%s/%s", g_root, store_tier_dir(tier)) >=
	    sizeof(dir_path))
		return -1;
	d = opendir(dir_path);
	if (d == NULL)
		return -1;
	while ((de = readdir(d)) != NULL) {
		char digest[STORE_SHA256_MAX];
		off_t size = -1;

		if (de->d_name[0] == '.')
			continue;
		if (!store_name_is_valid(tier, de->d_name))
			continue;
		if (store_resolve(tier, de->d_name, digest, sizeof(digest)) != STORE_OK)
			continue;
		store_blob_exists(digest, &size);
		rc = fn(tier, de->d_name, digest, size, ctx);
		if (rc != 0)
			break;
	}
	closedir(d);
	return rc;
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

static int collect_live(enum store_tier tier, const char *name, const char *digest, off_t size,
                        void *ctx)
{
	struct live_set *set = ctx;

	(void)tier;
	(void)name;
	(void)size;
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
	if (store_walk(STORE_TIER_PACKAGE, collect_live, &set) != 0 ||
	    store_walk(STORE_TIER_IMAGE, collect_live, &set) != 0) {
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
