#include "conf.h"
#include "http.h"
#include "importer.h"
#include "json.h"
#include "linux_compat.h"
#include "manifest.h"
#include "store.h"
#include "version.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_EVENTS 64
#define READ_BUF 65536
#define HDR_MAX 1024

/*
 * One sendfile() call moves at most this much. On a non-blocking
 * socket sendfile returns as soon as the socket buffer fills, so this
 * is not what bounds a slow client -- it bounds a FAST one, which
 * could otherwise sit in a single syscall pushing hundreds of MB into
 * a large window while every other connection waits.
 */
#define SENDFILE_CHUNK (4 * 1024 * 1024)

/*
 * The daemon sets no timeout of any kind on an artifact fetch: not
 * --max-time, not --connect-timeout, and no retry. A response that
 * stalls forever therefore does not fail on the host, it wedges that
 * host's fetch job indefinitely. Bounding the transfer here is the
 * only place it can be bounded at all.
 */
#define IDLE_TIMEOUT_MS 60000
#define XFER_TIMEOUT_MS 300000

#define UPLOAD_MAX_BYTES (16LL * 1024 * 1024 * 1024)

/*
 * A small in-memory ring of what the server has been doing, so an
 * operator can watch it without shelling in for journalctl. Everything
 * written here also goes to stderr, which systemd captures -- the ring
 * is a convenience, never the record.
 *
 * Bounded and overwritten oldest-first on purpose: this must not be a
 * place where memory grows with traffic, and a registry that ran out of
 * memory keeping a log of serving artifacts would be an absurd way to
 * fail.
 */
#define LOG_RING 512
#define LOG_TEXT_MAX 200

struct log_entry {
	long long seq;
	long long wall_ms;
	char level[8];
	char text[LOG_TEXT_MAX];
};

enum conn_kind { CONN_LISTENER, CONN_CLIENT, CONN_HASH_CHILD, CONN_IMPORT_CHILD, CONN_DEAD };

enum conn_state {
	CONN_READ_REQUEST,
	CONN_RECV_BODY,
	CONN_HASHING,
	CONN_SEND_HEADER,
	CONN_SEND_BODY
};

struct conn {
	enum conn_kind kind;
	enum conn_state state;
	int fd;
	struct http_conn http;

	char hdr[HDR_MAX];
	size_t hdr_len;
	size_t hdr_off;

	int head_only;

	int blob_fd;
	off_t body_off;
	off_t body_len;

	char *out_buf;
	size_t out_len;
	size_t out_off;

	int up_fd;
	char up_path[PATH_MAX];
	char up_digest[STORE_SHA256_MAX];
	char up_name[STORE_NAME_MAX];
	long up_expect;
	long up_received;

	pid_t child_pid;
	int child_out;
	struct conn *owner;
	struct conn *child;

	long long last_ms;
	char note[16];
	struct conn *next;
};

static struct conf g_conf;
static int g_epfd = -1;
static volatile sig_atomic_t g_stop;
static struct conn *g_conns;
static struct conn *g_pending_free;
static long long g_started_ms;
static long long g_served_bytes;
static long g_requests;
static long g_artifact_hits;
static long g_artifact_misses;
static long g_artifact_aliases;

static struct log_entry g_log[LOG_RING];
static long long g_log_seq;

static int g_import_running;
static pid_t g_import_pid;
static long long g_import_started_ms;
static int g_import_last_status = -1;

static void conn_close(struct conn *cc);

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long long wall_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void server_log(const char *level, const char *fmt, ...)
{
	struct log_entry *e = &g_log[g_log_seq % LOG_RING];
	va_list ap;

	e->seq = ++g_log_seq;
	e->wall_ms = wall_ms();
	snprintf(e->level, sizeof(e->level), "%s", level);
	va_start(ap, fmt);
	vsnprintf(e->text, sizeof(e->text), fmt, ap);
	va_end(ap);
	/* Also to stderr, so the journal stays the durable copy. */
	fprintf(stderr, "cixcached: %s\n", e->text);
}

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

/* ---- connection bookkeeping ---- */

static struct conn *conn_new(enum conn_kind kind, int fd)
{
	struct conn *cc = calloc(1, sizeof(*cc));

	if (cc == NULL)
		return NULL;
	cc->kind = kind;
	cc->fd = fd;
	cc->blob_fd = -1;
	cc->up_fd = -1;
	cc->child_out = -1;
	cc->child_pid = -1;
	cc->last_ms = now_ms();
	http_conn_init(&cc->http);
	cc->next = g_conns;
	g_conns = cc;
	return cc;
}

static void conn_unlink(struct conn *cc)
{
	struct conn **p = &g_conns;

	while (*p != NULL) {
		if (*p == cc) {
			*p = cc->next;
			return;
		}
		p = &(*p)->next;
	}
}

/*
 * Teardown is deferred: a conn can be torn down while its epoll batch
 * is still being walked, and freeing it there would leave the loop
 * dispatching against freed memory. The same reason the sibling
 * daemon keeps a pending-free list.
 */
static void conn_close(struct conn *cc)
{
	if (cc->kind == CONN_DEAD)
		return;
	if (cc->fd >= 0) {
		cix_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
		close(cc->fd);
		cc->fd = -1;
	}
	if (cc->blob_fd >= 0) {
		close(cc->blob_fd);
		cc->blob_fd = -1;
	}
	if (cc->up_fd >= 0) {
		close(cc->up_fd);
		cc->up_fd = -1;
	}
	if (cc->child_out >= 0) {
		close(cc->child_out);
		cc->child_out = -1;
	}
	if (cc->up_path[0] != '\0') {
		unlink(cc->up_path);
		cc->up_path[0] = '\0';
	}
	if (cc->child != NULL) {
		cc->child->owner = NULL;
		conn_close(cc->child);
		cc->child = NULL;
	}
	if (cc->owner != NULL)
		cc->owner->child = NULL;
	free(cc->out_buf);
	cc->out_buf = NULL;
	http_conn_free(&cc->http);
	cc->kind = CONN_DEAD;
	conn_unlink(cc);
	cc->next = g_pending_free;
	g_pending_free = cc;
}

static void drain_pending_free(void)
{
	while (g_pending_free != NULL) {
		struct conn *cc = g_pending_free;

		g_pending_free = cc->next;
		free(cc);
	}
}

static int epoll_set(struct conn *cc, uint32_t events)
{
	struct cix_epoll_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.events = events;
	ev.data.ptr = cc;
	return cix_epoll_ctl(g_epfd, EPOLL_CTL_MOD, cc->fd, &ev);
}

static int epoll_add(struct conn *cc, uint32_t events)
{
	struct cix_epoll_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.events = events;
	ev.data.ptr = cc;
	return cix_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev);
}

/* ---- responses ---- */

/*
 * True for the endpoints and assets the dashboard fetches on a timer.
 * Artifact paths are never observer paths, whoever requests them.
 */
static int is_observer_path(const char *path)
{
	if (strncmp(path, "/api/v1/", 8) == 0)
		return 1;
	if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0 ||
	    strcmp(path, "/app.js") == 0 || strcmp(path, "/style.css") == 0 ||
	    strcmp(path, "/favicon.svg") == 0)
		return 1;
	return 0;
}

static void begin_response(struct conn *cc, int status, const char *content_type, const char *extra,
                           long long content_length)
{
	int n = http_format_header(cc->hdr, sizeof(cc->hdr), status, http_status_text(status),
	                           content_type, content_length, extra);

	if (n < 0) {
		conn_close(cc);
		return;
	}
	/*
	 * Log what the registry does as a registry, not what the dashboard
	 * does while watching it. A dashboard polling four endpoints every
	 * two seconds produces two lines a second forever, which buries
	 * the artifact traffic the log exists to show -- the observer
	 * effect, and it made the log useless the first time it was
	 * actually looked at.
	 *
	 * So successful dashboard and asset requests are not recorded.
	 * Anything that failed still is, whoever asked: a 4xx on an API
	 * call is a real event even when the dashboard caused it.
	 */
	if (cc->http.method[0] != '\0' && (status >= 400 || !is_observer_path(cc->http.path)))
		server_log(status >= 400 ? "warn" : "info", "%s %s -> %d %s%lld bytes",
		           cc->http.method, cc->http.path, status,
		           cc->note[0] != '\0' ? cc->note : "", content_length);
	cc->hdr_len = (size_t)n;
	cc->hdr_off = 0;
	cc->state = CONN_SEND_HEADER;
	cc->last_ms = now_ms();
	epoll_set(cc, EPOLLOUT);
}

static void respond_buffer(struct conn *cc, int status, const char *content_type, char *body,
                           size_t body_len)
{
	free(cc->out_buf);
	cc->out_buf = body;
	cc->out_len = body_len;
	cc->out_off = 0;
	begin_response(cc, status, content_type, NULL, (long long)body_len);
}

static void respond_error(struct conn *cc, int status, const char *msg)
{
	struct json_writer w;
	char *body;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "error");
	jw_str(&w, msg);
	jw_obj_close(&w);
	body = malloc(w.len + 1);
	if (body == NULL) {
		jw_free(&w);
		conn_close(cc);
		return;
	}
	memcpy(body, w.buf, w.len);
	body[w.len] = '\0';
	respond_buffer(cc, status, "application/json", body, w.len);
	jw_free(&w);
}

static void respond_json(struct conn *cc, int status, struct json_writer *w)
{
	char *body = malloc(w->len + 1);

	if (body == NULL) {
		conn_close(cc);
		return;
	}
	memcpy(body, w->buf, w->len);
	body[w->len] = '\0';
	respond_buffer(cc, status, "application/json", body, w->len);
}

/* ---- auth ---- */

static int bearer_ok(const struct http_request *req, const char *expect)
{
	char hdr[CONF_TOKEN_MAX + 32];
	const char *got;

	if (expect == NULL || expect[0] == '\0')
		return 1;
	if (http_find_header(req->headers, req->headers_len, "Authorization", hdr, sizeof(hdr)) < 0)
		return 0;
	got = strncmp(hdr, "Bearer ", 7) == 0 ? hdr + 7 : hdr;
	return strcmp(got, expect) == 0;
}

/* ---- artifact serving ---- */

static void serve_artifact(struct conn *cc, const struct http_request *req, const char *name,
                           int head_only)
{
	char extra[STORE_SHA256_MAX + 32];
	char digest[STORE_SHA256_MAX];
	char canonical[STORE_NAME_MAX];
	char served[STORE_NAME_MAX];
	enum store_error e;
	off_t size = 0;
	int fd = -1;

	if (!bearer_ok(req, g_conf.pull_token)) {
		respond_error(cc, 401, "authentication required");
		return;
	}
	e = store_open(name, &fd, &size, digest, sizeof(digest), served, sizeof(served));
	if (e != STORE_OK) {
		/*
		 * A miss is the ordinary answer, not a fault: the host builds
		 * from source instead, and an artifact server must never be
		 * able to fail a build.
		 *
		 * But that is exactly what makes a misconfigured registry
		 * invisible. A wrong layout looks like a working cache that
		 * simply never gets used -- there is no failure to notice.
		 * This was found live on 2026-08-26 (LAYOUT-CORRECTION.md):
		 * packages were served under /packages/ while every host
		 * computes them at the root, so every request 404'd and every
		 * host quietly rebuilt from source. It surfaced only because
		 * someone was watching an access log during a test install.
		 *
		 * So every miss names the exact URL that missed, and hits and
		 * misses are counted separately. "Not cached" and "cached but
		 * unreachable" must not look the same from the outside.
		 */
		/*
		 * Ambiguity is not a miss. A miss means "build from source"
		 * and is harmless; this means the store holds this name for
		 * more than one machine and will not guess between them. It
		 * has to be loud, because it is the one case where answering
		 * would be worse than failing.
		 */
		if (e == STORE_ERR_AMBIGUOUS) {
			snprintf(cc->note, sizeof(cc->note), "AMBIG ");
			server_log("err", "ambiguous: %s exists for more than one architecture -- the "
			                  "requester must name one",
			           name);
			respond_error(cc, 409, store_error_str(e));
			return;
		}
		g_artifact_misses++;
		snprintf(cc->note, sizeof(cc->note), "MISS ");
		respond_error(cc, e == STORE_ERR_INVALID_NAME ? 400 : 404, store_error_str(e));
		return;
	}
	g_artifact_hits++;
	/*
	 * A hit under a non-canonical name serves the right bytes -- the
	 * two names are one entry -- but it means a recipe out there is
	 * still spelling it the old way. Nothing else would ever report
	 * that, precisely because the alias works, so say it here. The
	 * note is too small for a name, so the detail goes to the log.
	 */
	if (store_canonical_name(name, canonical, sizeof(canonical)) == 1 ||
	    strcmp(served, name) != 0) {
		g_artifact_aliases++;
		snprintf(cc->note, sizeof(cc->note), "ALIAS ");
		server_log("warn", "alias: %s served as %s -- the requester is using a "
		                   "non-canonical name", name, served);
	} else {
		snprintf(cc->note, sizeof(cc->note), "HIT ");
	}
	snprintf(extra, sizeof(extra), "X-Cix-Sha256: %s\r\n", digest);
	cc->head_only = head_only;
	if (head_only) {
		close(fd);
		begin_response(cc, 200, "application/gzip", extra, (long long)size);
		return;
	}
	cc->blob_fd = fd;
	cc->body_off = 0;
	cc->body_len = size;
	begin_response(cc, 200, "application/gzip", extra, (long long)size);
}

/* ---- push ---- */

static void finish_upload(struct conn *cc)
{
	char digest[STORE_SHA256_MAX];
	enum store_error e;
	char buf[128];
	size_t got = 0;
	int status = 0;

	/*
	 * The pidfd fired, so sha256sum has already exited and its whole
	 * output -- 64 hex digits and a filename -- is sitting in the pipe
	 * buffer. Reading it here cannot block on a live writer.
	 */
	while (got < sizeof(buf) - 1) {
		ssize_t n = read(cc->child_out, buf + got, sizeof(buf) - 1 - got);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		got += (size_t)n;
	}
	buf[got] = '\0';
	if (cc->child_pid > 0)
		waitpid(cc->child_pid, &status, 0);
	close(cc->child_out);
	cc->child_out = -1;
	cc->child_pid = -1;

	if (got < STORE_SHA256_HEX_LEN) {
		respond_error(cc, 500, "could not hash uploaded body");
		return;
	}
	memcpy(digest, buf, STORE_SHA256_HEX_LEN);
	digest[STORE_SHA256_HEX_LEN] = '\0';
	if (!store_digest_is_valid(digest)) {
		respond_error(cc, 500, "could not hash uploaded body");
		return;
	}
	if (strcasecmp(digest, cc->up_digest) != 0) {
		/*
		 * Caught at the door rather than by every puller afterwards.
		 * The consumer still verifies against its recipe -- this
		 * server is not a trust boundary and this check does not make
		 * it one; it just refuses to store bytes nobody asked for.
		 */
		server_log("warn", "push %s rejected: declared %s, body hashes %s", cc->up_name,
		           cc->up_digest, digest);
		respond_error(cc, 400, "body does not match declared sha256");
		return;
	}
	e = store_blob_adopt(cc->up_path, digest);
	cc->up_path[0] = '\0'; /* adopted or unlinked -- either way not ours now */
	if (e != STORE_OK) {
		respond_error(cc, 500, store_error_str(e));
		return;
	}
	e = store_publish(cc->up_name, digest);
	if (e == STORE_ERR_CONFLICT) {
		respond_error(cc, 409, "already published with a different sha256");
		return;
	}
	if (e != STORE_OK) {
		respond_error(cc, 500, store_error_str(e));
		return;
	}
	server_log("info", "published %s -> %s", cc->up_name, digest);
	begin_response(cc, 201, "application/json", NULL, 0);
}

static void start_hash_child(struct conn *cc)
{
	struct conn *child;
	int pipefd[2];
	int pidfd;
	pid_t pid;

	close(cc->up_fd);
	cc->up_fd = -1;
	if (pipe(pipefd) != 0) {
		respond_error(cc, 500, "cannot hash upload");
		return;
	}
	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		respond_error(cc, 500, "cannot hash upload");
		return;
	}
	if (pid == 0) {
		char *argv[] = { (char *)STORE_SHA256SUM_BIN, cc->up_path, NULL };
		extern char **environ;

		close(pipefd[0]);
		if (dup2(pipefd[1], STDOUT_FILENO) < 0)
			_exit(127);
		close(pipefd[1]);
		execve(STORE_SHA256SUM_BIN, argv, environ);
		_exit(127);
	}
	close(pipefd[1]);
	/*
	 * Hashing 2.7 GB takes on the order of ten seconds -- far too long
	 * to block a single-threaded reactor. Watch the child as a pidfd
	 * instead, the same way the sibling daemon watches its own package
	 * and image fetch children.
	 */
	pidfd = cix_pidfd_open(pid);
	if (pidfd < 0) {
		close(pipefd[0]);
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		respond_error(cc, 500, "cannot watch hash child");
		return;
	}
	child = conn_new(CONN_HASH_CHILD, pidfd);
	if (child == NULL) {
		close(pidfd);
		close(pipefd[0]);
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		respond_error(cc, 500, "out of memory");
		return;
	}
	child->owner = cc;
	cc->child = child;
	cc->child_pid = pid;
	cc->child_out = pipefd[0];
	cc->state = CONN_HASHING;
	cc->last_ms = now_ms();
	epoll_set(cc, 0);
	if (epoll_add(child, EPOLLIN) != 0) {
		respond_error(cc, 500, "cannot watch hash child");
		return;
	}
}

static void begin_upload(struct conn *cc, const struct http_request *req, const char *name)
{
	char digest[STORE_SHA256_MAX + 8];

	if (!bearer_ok(req, g_conf.push_token)) {
		/*
		 * Auth asymmetry is deliberate: pull may be open because the
		 * consumer verifies every byte against its recipe anyway, but
		 * an open push lets anyone fill the disk or plant blobs that
		 * every puller then has to reject.
		 */
		respond_error(cc, 401, "push requires a bearer token");
		return;
	}
	if (!store_name_is_valid(name)) {
		respond_error(cc, 400, "invalid artifact name");
		return;
	}
	if (req->content_length <= 0) {
		respond_error(cc, 411, "content-length required");
		return;
	}
	if ((long long)req->content_length > UPLOAD_MAX_BYTES) {
		respond_error(cc, 413, "upload too large");
		return;
	}
	if (http_find_header(req->headers, req->headers_len, "X-Cix-Sha256", digest, sizeof(digest)) <
	        0 ||
	    !store_digest_is_valid(digest)) {
		respond_error(cc, 400, "X-Cix-Sha256 header required, 64 lowercase hex");
		return;
	}
	snprintf(cc->up_digest, sizeof(cc->up_digest), "%s", digest);
	snprintf(cc->up_name, sizeof(cc->up_name), "%s", name);
	cc->up_expect = req->content_length;
	cc->up_received = 0;
	if (store_tmp_path(cc->up_path, sizeof(cc->up_path)) != 0) {
		respond_error(cc, 500, "cannot stage upload");
		return;
	}
	cc->up_fd = open(cc->up_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (cc->up_fd < 0) {
		cc->up_path[0] = '\0';
		respond_error(cc, 500, "cannot stage upload");
		return;
	}
	cc->state = CONN_RECV_BODY;
	/* Body bytes that rode in with the headers are already buffered. */
	if (req->body_len > 0) {
		if (write(cc->up_fd, req->body, req->body_len) != (ssize_t)req->body_len) {
			respond_error(cc, 500, "cannot stage upload");
			return;
		}
		cc->up_received = (long)req->body_len;
	}
	if (cc->up_received >= cc->up_expect)
		start_hash_child(cc);
}

/* ---- api ---- */

struct list_ctx {
	struct json_writer *w;
	/* Set per entry before list_entry(), which has a fixed signature. */
	int version_rank;
	long count;
	/*
	 * Published entries carrying no architecture. Every push from a
	 * daemon that does not send one creates another, and each is a
	 * name that cannot be told apart from a build for another machine
	 * until it is stamped. Counted because it is otherwise invisible:
	 * they resolve and serve correctly right up until the day they do
	 * not (ADR-0005, and #6).
	 */
	long unstamped;
	long long bytes;
	/*
	 * Distinct artifact names, as opposed to published files. This
	 * store holds four zlibs and three greps; "88 artifacts" and
	 * "N packages" are different facts and an operator wants both.
	 */
	char *names;
	size_t name_count;
	size_t name_cap;
};

static int name_cmp(const void *a, const void *b)
{
	return strcmp((const char *)a, (const char *)b);
}

static void remember_name(struct list_ctx *lc, const char *short_name)
{
	if (lc->name_count == lc->name_cap) {
		size_t cap = lc->name_cap != 0 ? lc->name_cap * 2 : 64;
		char *grown = realloc(lc->names, cap * STORE_NAME_MAX);

		if (grown == NULL)
			return;
		lc->names = grown;
		lc->name_cap = cap;
	}
	snprintf(lc->names + lc->name_count * STORE_NAME_MAX, STORE_NAME_MAX, "%s", short_name);
	lc->name_count++;
}

static long distinct_names(struct list_ctx *lc)
{
	long distinct = 0;
	size_t i;

	if (lc->name_count == 0)
		return 0;
	qsort(lc->names, lc->name_count, STORE_NAME_MAX, name_cmp);
	for (i = 0; i < lc->name_count; i++)
		if (i == 0 || strcmp(lc->names + i * STORE_NAME_MAX,
		                     lc->names + (i - 1) * STORE_NAME_MAX) != 0)
			distinct++;
	return distinct;
}

static int list_entry(const char *name, const char *digest, off_t size, time_t mtime, void *ctx)
{
	struct list_ctx *lc = ctx;

	char short_name[STORE_NAME_MAX];
	char version[STORE_NAME_MAX];
	char arch[STORE_NAME_MAX];
	int release = 1;

	/*
	 * No url field: the URL is the name, at the root of base_url.
	 * Sending both invites them to disagree, and the client can derive
	 * one from the other.
	 *
	 * artifact, version and release are split for display only --
	 * name stays the authoritative key.
	 *
	 * release is reported as its own number so no consumer has to
	 * re-parse the string to get at it, and version is upstream's
	 * alone. A name carrying no release reports 1, which is what it
	 * means.
	 */
	store_split_display(name, short_name, sizeof(short_name), version, sizeof(version), &release,
	                    arch, sizeof(arch));
	jw_obj_open(lc->w);
	jw_key(lc->w, "name");
	jw_str(lc->w, name);
	jw_key(lc->w, "artifact");
	jw_str(lc->w, short_name);
	jw_key(lc->w, "version");
	jw_str(lc->w, version);
	jw_key(lc->w, "release");
	jw_int(lc->w, release);
	/*
	 * Empty for an artifact that names no architecture. Reported as
	 * absent rather than guessed, because the store genuinely does not
	 * know -- see ADR-0008.
	 */
	jw_key(lc->w, "arch");
	jw_str(lc->w, arch);
	/*
	 * Position in version order, computed here so there is exactly one
	 * implementation of the ordering rules (store_version_cmp). A
	 * browser sorting on this number cannot disagree with the server
	 * about what "newer" means, which a second comparator in
	 * JavaScript eventually would.
	 */
	jw_key(lc->w, "version_rank");
	jw_int(lc->w, lc->version_rank);
	jw_key(lc->w, "sha256");
	jw_str(lc->w, digest);
	jw_key(lc->w, "bytes");
	jw_int(lc->w, (long long)size);
	jw_key(lc->w, "modified");
	jw_int(lc->w, (long long)mtime);
	jw_obj_close(lc->w);
	lc->count++;
	if (size > 0)
		lc->bytes += (long long)size;
	return 0;
}

static int count_entry(const char *name, const char *digest, off_t size, time_t mtime, void *ctx)
{
	struct list_ctx *lc = ctx;
	char short_name[STORE_NAME_MAX];
	char version[STORE_NAME_MAX];

	(void)digest;
	(void)mtime;
	store_split_display(name, short_name, sizeof(short_name), version, sizeof(version), NULL, NULL,
	                    0);
	if (store_arch_of(name, NULL, 0) == NULL)
		lc->unstamped++;
	remember_name(lc, short_name);
	lc->count++;
	if (size > 0)
		lc->bytes += (long long)size;
	return 0;
}

/*
 * Reads one query parameter out of a raw path. No decoding and no
 * repeated keys: the only callers want a small integer.
 */
static int query_int(const char *path, const char *key, long long *out)
{
	const char *q = strchr(path, '?');
	size_t klen = strlen(key);

	if (q == NULL)
		return -1;
	q++;
	while (*q != '\0') {
		if (strncmp(q, key, klen) == 0 && q[klen] == '=') {
			*out = strtoll(q + klen + 1, NULL, 10);
			return 0;
		}
		q = strchr(q, '&');
		if (q == NULL)
			break;
		q++;
	}
	return -1;
}

/*
 * Entries newer than ?after=<seq>. A client polls with the last seq it
 * saw and gets only what it has not, so tailing costs one small
 * response per interval rather than the whole ring each time.
 */
static void api_log(struct conn *cc, const char *raw_path)
{
	struct json_writer w;
	long long after = 0;
	long long oldest;
	long long i;

	query_int(raw_path, "after", &after);
	oldest = g_log_seq > LOG_RING ? g_log_seq - LOG_RING : 0;
	if (after < oldest)
		after = oldest;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "entries");
	jw_arr_open(&w);
	for (i = after + 1; i <= g_log_seq; i++) {
		const struct log_entry *e = &g_log[(i - 1) % LOG_RING];

		if (e->seq != i)
			continue;
		jw_obj_open(&w);
		jw_key(&w, "seq");
		jw_int(&w, e->seq);
		jw_key(&w, "time_ms");
		jw_int(&w, e->wall_ms);
		jw_key(&w, "level");
		jw_str(&w, e->level);
		jw_key(&w, "text");
		jw_str(&w, e->text);
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	jw_key(&w, "seq");
	jw_int(&w, g_log_seq);
	jw_obj_close(&w);
	respond_json(cc, 200, &w);
	jw_free(&w);
}

static void api_status(struct conn *cc)
{
	struct list_ctx pkgs;
	struct json_writer w;

	memset(&pkgs, 0, sizeof(pkgs));
	store_walk(count_entry, &pkgs);

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "build_version");
	jw_str(&w, CIXCACHE_BUILD_VERSION);
	jw_key(&w, "build_time");
	jw_str(&w, CIXCACHE_BUILD_TIME);
	jw_key(&w, "root");
	jw_str(&w, store_root());
	jw_key(&w, "uptime_seconds");
	jw_int(&w, (now_ms() - g_started_ms) / 1000);
	jw_key(&w, "requests");
	jw_int(&w, g_requests);
	jw_key(&w, "artifact_hits");
	jw_int(&w, g_artifact_hits);
	jw_key(&w, "artifact_misses");
	jw_int(&w, g_artifact_misses);
	jw_key(&w, "artifact_aliases");
	jw_int(&w, g_artifact_aliases);
	jw_key(&w, "served_bytes");
	jw_int(&w, g_served_bytes);
	jw_key(&w, "unstamped");
	jw_int(&w, pkgs.unstamped);
	jw_key(&w, "packages");
	jw_int(&w, pkgs.count);
	jw_key(&w, "unique_packages");
	jw_int(&w, distinct_names(&pkgs));
	jw_key(&w, "package_bytes");
	jw_int(&w, pkgs.bytes);
	jw_key(&w, "pull_open");
	jw_bool(&w, g_conf.pull_token[0] == '\0');
	jw_key(&w, "push_configured");
	jw_bool(&w, g_conf.push_token[0] != '\0');
	jw_obj_close(&w);
	respond_json(cc, 200, &w);
	jw_free(&w);
	free(pkgs.names);
}

static void api_artifacts(struct conn *cc)
{
	struct json_writer w;
	struct list_ctx lc;

	jw_init(&w);
	memset(&lc, 0, sizeof(lc));
	lc.w = &w;
	jw_obj_open(&w);
	jw_key(&w, "artifacts");
	jw_arr_open(&w);
	/*
	 * Most recently published first. The dashboard is search-first, so
	 * finding a known name is the search box's job and the list is
	 * free to answer the other question an operator has: what landed.
	 *
	 * Sorted here rather than in the dashboard and the CLI separately,
	 * so every consumer of this endpoint sees one order.
	 */
	{
		struct store_entry *ents = NULL;
		int n = store_list(&ents);
		int i;

		if (n > 0) {
			/*
			 * Ranked before the output order is chosen, over the
			 * whole set, so the rank means the same thing however
			 * the client then filters or re-sorts.
			 */
			store_rank_versions(ents, n);
			qsort(ents, (size_t)n, sizeof(*ents), store_cmp_newest);
			for (i = 0; i < n; i++) {
				lc.version_rank = ents[i].version_rank;
				list_entry(ents[i].name, ents[i].digest, ents[i].size, ents[i].mtime, &lc);
			}
		}
		free(ents);
	}
	jw_arr_close(&w);
	jw_key(&w, "count");
	jw_int(&w, lc.count);
	jw_key(&w, "bytes");
	jw_int(&w, lc.bytes);
	jw_obj_close(&w);
	respond_json(cc, 200, &w);
	jw_free(&w);
	free(lc.names);
}

static void api_gc(struct conn *cc, int dry_run)
{
	struct json_writer w;
	long long freed = 0;
	int removed;

	removed = store_gc(dry_run, &freed);
	if (removed >= 0)
		server_log("info", "gc%s removed %d blobs, %lld bytes", dry_run ? " (dry run)" : "",
		           removed, freed);
	if (removed < 0) {
		respond_error(cc, 500, "garbage collection failed");
		return;
	}
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "dry_run");
	jw_bool(&w, dry_run);
	jw_key(&w, "removed");
	jw_int(&w, removed);
	jw_key(&w, "bytes_freed");
	jw_int(&w, freed);
	jw_obj_close(&w);
	respond_json(cc, 200, &w);
	jw_free(&w);
}

static void api_import_status(struct conn *cc)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "running");
	jw_bool(&w, g_import_running);
	jw_key(&w, "elapsed_seconds");
	jw_int(&w, g_import_running ? (now_ms() - g_import_started_ms) / 1000 : 0);
	jw_key(&w, "last_exit_status");
	jw_int(&w, g_import_last_status);
	jw_obj_close(&w);
	respond_json(cc, 200, &w);
	jw_free(&w);
}

static void api_import_start(struct conn *cc)
{
	struct conn *child;
	int pidfd;
	pid_t pid;

	if (g_import_running) {
		respond_error(cc, 409, "an import is already running");
		return;
	}
	pid = fork();
	if (pid < 0) {
		respond_error(cc, 500, "cannot fork importer");
		return;
	}
	if (pid == 0) {
		char manifest[PATH_MAX];

		snprintf(manifest, sizeof(manifest), "%s/MANIFEST.json", store_root());
		_exit(importer_run(store_root(), manifest, 0, NULL) == 0 ? 0 : 1);
	}
	pidfd = cix_pidfd_open(pid);
	if (pidfd < 0) {
		respond_error(cc, 500, "cannot watch importer");
		return;
	}
	child = conn_new(CONN_IMPORT_CHILD, pidfd);
	if (child == NULL) {
		close(pidfd);
		respond_error(cc, 500, "out of memory");
		return;
	}
	child->child_pid = pid;
	epoll_add(child, EPOLLIN);
	g_import_running = 1;
	g_import_pid = pid;
	g_import_started_ms = now_ms();
	begin_response(cc, 202, "application/json", NULL, 0);
}

static void serve_manifest(struct conn *cc)
{
	struct json_writer w;

	jw_init(&w);
	manifest_write_json(&w);
	respond_json(cc, 200, &w);
	jw_free(&w);
}

/* ---- static assets ---- */

static const char *content_type_for(const char *path)
{
	const char *dot = strrchr(path, '.');

	if (dot == NULL)
		return "application/octet-stream";
	if (strcmp(dot, ".html") == 0)
		return "text/html; charset=utf-8";
	if (strcmp(dot, ".js") == 0)
		return "application/javascript";
	if (strcmp(dot, ".css") == 0)
		return "text/css";
	if (strcmp(dot, ".svg") == 0)
		return "image/svg+xml";
	if (strcmp(dot, ".json") == 0)
		return "application/json";
	return "application/octet-stream";
}

static void serve_static(struct conn *cc, const char *req_path, int head_only)
{
	char path[PATH_MAX];
	struct stat st;
	char *body;
	int fd;

	if (strstr(req_path, "..") != NULL) {
		respond_error(cc, 400, "bad path");
		return;
	}
	if (strcmp(req_path, "/") == 0)
		req_path = "/index.html";
	if ((size_t)snprintf(path, sizeof(path), "%s%s", g_conf.web_root, req_path) >= sizeof(path)) {
		respond_error(cc, 400, "bad path");
		return;
	}
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		respond_error(cc, 404, "no such endpoint");
		return;
	}
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
		close(fd);
		respond_error(cc, 404, "no such endpoint");
		return;
	}
	/*
	 * HEAD answers exactly as GET would, minus the body. Returning 404
	 * to a HEAD on a file that GETs 200 makes a proxy or a health check
	 * read the asset as missing, and costs nothing to get right: the
	 * size and type are already known, so the body is never read.
	 */
	if (head_only) {
		close(fd);
		cc->head_only = 1;
		begin_response(cc, 200, content_type_for(path), NULL, (long long)st.st_size);
		return;
	}

	/*
	 * Dashboard assets are read whole, unlike artifacts: they are tens
	 * of KB and are the only thing in this server small enough that
	 * buffering is simpler than streaming.
	 */
	body = malloc((size_t)st.st_size + 1);
	if (body == NULL) {
		close(fd);
		respond_error(cc, 500, "out of memory");
		return;
	}
	if (read(fd, body, (size_t)st.st_size) != (ssize_t)st.st_size) {
		close(fd);
		free(body);
		respond_error(cc, 500, "short read");
		return;
	}
	close(fd);
	respond_buffer(cc, 200, content_type_for(path), body, (size_t)st.st_size);
}

/* ---- routing ---- */

static int ends_with_targz(const char *s)
{
	size_t len = strlen(s);

	return len > 7 && strcmp(s + len - 7, ".tar.gz") == 0;
}

static void dispatch(struct conn *cc, const struct http_request *req)
{
	char path[HTTP_MAX_PATH];
	int is_get;
	int is_head;
	char *q;

	g_requests++;
	/*
	 * Per-request state, on a connection that is reused. Both are set
	 * only by the paths that need them, so without a reset here a
	 * keep-alive GET following a HEAD would inherit head_only and drop
	 * its body, and a request would inherit the previous one's log
	 * note and be recorded as something it was not.
	 */
	cc->head_only = 0;
	cc->note[0] = '\0';
	snprintf(path, sizeof(path), "%s", req->path);
	q = strchr(path, '?');
	if (q != NULL)
		*q = '\0';
	if (http_path_decode(path) != 0) {
		respond_error(cc, 400, "bad percent-encoding in path");
		return;
	}
	is_get = strcmp(req->method, "GET") == 0;
	is_head = strcmp(req->method, "HEAD") == 0;

	/*
	 * Artifact paths are the only ones that end .tar.gz, so the
	 * machine-facing namespace and the operator-facing one cannot
	 * collide however the store is filled.
	 */
	if (ends_with_targz(path)) {
		const char *name = path + 1;

		/*
		 * Every artifact lives at the root of base_url. A path with a
		 * directory component -- /images/... included -- names nothing
		 * this registry has, so it misses like any other unknown name.
		 */
		if (strchr(name, '/') != NULL) {
			respond_error(cc, 404, "no such artifact");
			return;
		}
		if (is_get || is_head) {
			serve_artifact(cc, req, name, is_head);
			return;
		}
		if (strcmp(req->method, "PUT") == 0) {
			begin_upload(cc, req, name);
			return;
		}
		if (strcmp(req->method, "DELETE") == 0) {
			enum store_error e;

			if (!bearer_ok(req, g_conf.push_token)) {
				respond_error(cc, 401, "push requires a bearer token");
				return;
			}
			e = store_unpublish(name);
			if (e == STORE_OK)
				server_log("info", "unpublished %s", name);
			if (e != STORE_OK) {
				respond_error(cc, e == STORE_ERR_NOT_FOUND ? 404 : 400, store_error_str(e));
				return;
			}
			begin_response(cc, 204, "application/json", NULL, 0);
			return;
		}
		respond_error(cc, 405, "method not allowed");
		return;
	}

	if (is_get && strcmp(path, "/MANIFEST.json") == 0) {
		serve_manifest(cc);
		return;
	}

	/*
	 * Everything under /api/v1 is operator-facing observability and
	 * nothing more. DESIGN.md section 2 is explicit that this registry
	 * is a cache and never a catalogue: if any host code path ever had
	 * to ask it "does this package exist", the two-URL split would
	 * already be broken. Hosts only ever GET or HEAD one exact
	 * artifact name; a dashboard listing is for people.
	 */
	if (strncmp(path, "/api/v1/", 8) == 0) {
		const char *ep = path + 8;

		if (is_get && strcmp(ep, "status") == 0) {
			api_status(cc);
			return;
		}
		if (is_get && strcmp(ep, "artifacts") == 0) {
			api_artifacts(cc);
			return;
		}
		if (is_get && strcmp(ep, "log") == 0) {
			api_log(cc, req->path);
			return;
		}
		if (is_get && strcmp(ep, "import-status") == 0) {
			api_import_status(cc);
			return;
		}
		if (strcmp(req->method, "POST") == 0 && strcmp(ep, "gc") == 0) {
			if (!bearer_ok(req, g_conf.push_token)) {
				respond_error(cc, 401, "gc requires a bearer token");
				return;
			}
			api_gc(cc, 0);
			return;
		}
		if (is_get && strcmp(ep, "gc") == 0) {
			api_gc(cc, 1);
			return;
		}
		if (strcmp(req->method, "POST") == 0 && strcmp(ep, "import") == 0) {
			if (!bearer_ok(req, g_conf.push_token)) {
				respond_error(cc, 401, "import requires a bearer token");
				return;
			}
			api_import_start(cc);
			return;
		}
		respond_error(cc, 404, "no such endpoint");
		return;
	}

	if (is_get || is_head) {
		serve_static(cc, path, is_head);
		return;
	}
	respond_error(cc, 404, "no such endpoint");
}

/* ---- event handling ---- */

static void handle_read(struct conn *cc)
{
	char buf[READ_BUF];

	for (;;) {
		struct http_request req;
		ssize_t n;
		int rc;

		n = read(cc->fd, buf, sizeof(buf));
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			if (errno == EINTR)
				continue;
			conn_close(cc);
			return;
		}
		if (n == 0) {
			conn_close(cc);
			return;
		}
		cc->last_ms = now_ms();

		if (cc->state == CONN_RECV_BODY) {
			long remaining = cc->up_expect - cc->up_received;
			size_t take = (size_t)n;

			if ((long)take > remaining)
				take = (size_t)remaining;
			if (write(cc->up_fd, buf, take) != (ssize_t)take) {
				respond_error(cc, 500, "short write staging upload");
				return;
			}
			cc->up_received += (long)take;
			if (cc->up_received >= cc->up_expect) {
				start_hash_child(cc);
				return;
			}
			continue;
		}

		if (http_conn_feed(&cc->http, buf, (size_t)n) != 0) {
			respond_error(cc, 413, "request headers too large");
			return;
		}
		rc = http_conn_try_parse(&cc->http, &req);
		if (rc < 0) {
			respond_error(cc, 400, "malformed request");
			return;
		}
		if (rc == 0)
			continue;
		dispatch(cc, &req);
		return;
	}
}

static void handle_write(struct conn *cc)
{
	for (;;) {
		if (cc->state == CONN_SEND_HEADER) {
			ssize_t n = write(cc->fd, cc->hdr + cc->hdr_off, cc->hdr_len - cc->hdr_off);

			if (n < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					return;
				if (errno == EINTR)
					continue;
				conn_close(cc);
				return;
			}
			cc->hdr_off += (size_t)n;
			cc->last_ms = now_ms();
			if (cc->hdr_off < cc->hdr_len)
				continue;
			if (cc->head_only || (cc->blob_fd < 0 && cc->out_buf == NULL)) {
				conn_close(cc);
				return;
			}
			cc->state = CONN_SEND_BODY;
			continue;
		}
		if (cc->state != CONN_SEND_BODY) {
			conn_close(cc);
			return;
		}
		if (cc->blob_fd >= 0) {
			off_t remaining = cc->body_len - cc->body_off;
			size_t chunk = remaining > SENDFILE_CHUNK ? SENDFILE_CHUNK : (size_t)remaining;
			ssize_t n;

			if (remaining <= 0) {
				conn_close(cc);
				return;
			}
			n = sendfile(cc->fd, cc->blob_fd, &cc->body_off, chunk);
			if (n < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					return;
				if (errno == EINTR)
					continue;
				conn_close(cc);
				return;
			}
			if (n == 0) {
				conn_close(cc);
				return;
			}
			g_served_bytes += (long long)n;
			cc->last_ms = now_ms();
			if (cc->body_off >= cc->body_len) {
				conn_close(cc);
				return;
			}
			continue;
		}
		if (cc->out_buf != NULL) {
			ssize_t n = write(cc->fd, cc->out_buf + cc->out_off, cc->out_len - cc->out_off);

			if (n < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					return;
				if (errno == EINTR)
					continue;
				conn_close(cc);
				return;
			}
			cc->out_off += (size_t)n;
			g_served_bytes += (long long)n;
			cc->last_ms = now_ms();
			if (cc->out_off >= cc->out_len) {
				conn_close(cc);
				return;
			}
			continue;
		}
		conn_close(cc);
		return;
	}
}

static void accept_loop(struct conn *listener)
{
	for (;;) {
		struct sockaddr_in peer;
		socklen_t plen = sizeof(peer);
		struct conn *cc;
		int fd;

		fd = accept4(listener->fd, (struct sockaddr *)&peer, &plen,
		             SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (fd < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			if (errno == EINTR)
				continue;
			perror("accept4");
			return;
		}
		cc = conn_new(CONN_CLIENT, fd);
		if (cc == NULL) {
			close(fd);
			continue;
		}
		cc->state = CONN_READ_REQUEST;
		if (epoll_add(cc, EPOLLIN) != 0)
			conn_close(cc);
	}
}

static void sweep_timeouts(void)
{
	long long now = now_ms();
	struct conn *cc = g_conns;

	while (cc != NULL) {
		struct conn *next = cc->next;
		long long idle = now - cc->last_ms;

		if (cc->kind == CONN_CLIENT) {
			long long limit = (cc->state == CONN_SEND_BODY || cc->state == CONN_RECV_BODY ||
			                   cc->state == CONN_HASHING)
			                          ? XFER_TIMEOUT_MS
			                          : IDLE_TIMEOUT_MS;

			if (idle > limit) {
				fprintf(stderr, "cixcached: closing stalled connection after %lldms\n", idle);
				conn_close(cc);
			}
		}
		cc = next;
	}
}

static int create_listener(const char *bind_addr, int port)
{
	struct sockaddr_in addr;
	int one = 1;
	int fd;

	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	if (bind_addr == NULL || bind_addr[0] == '\0' || strcmp(bind_addr, "0.0.0.0") == 0)
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
	else if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
		fprintf(stderr, "cixcached: bad bind address '%s'\n", bind_addr);
		close(fd);
		return -1;
	}
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		fprintf(stderr, "cixcached: bind %s:%d: %s\n", bind_addr, port, strerror(errno));
		close(fd);
		return -1;
	}
	if (listen(fd, 128) != 0) {
		perror("listen");
		close(fd);
		return -1;
	}
	return fd;
}

static void usage(FILE *out)
{
	fprintf(out,
	        "usage: cixcached [--config=PATH] [--root=DIR] [--bind=ADDR] [--port=N]\n"
	        "                 [--web-root=DIR] [--push-token=TOK] [--pull-token=TOK]\n"
	        "       cixcached --import [--dry-run] [--root=DIR]\n"
	        "       cixcached --canonicalize [--dry-run] [--root=DIR]\n"
	        "       cixcached --set-arch=ARCH [--dry-run] [--root=DIR]\n"
	        "       cixcached --version\n");
}

int main(int argc, char **argv)
{
	const char *config_path = NULL;
	struct conn *listener;
	int do_import = 0;
	int do_canonicalize = 0;
	const char *set_arch = NULL;
	int dry_run = 0;
	int listen_fd;
	int i;

	conf_defaults(&g_conf);
	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--config=", 9) == 0)
			config_path = argv[i] + 9;
	}
	if (config_path != NULL && conf_load(&g_conf, config_path) != 0) {
		fprintf(stderr, "cixcached: cannot read config %s\n", config_path);
		return 1;
	}
	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--config=", 9) == 0)
			continue;
		else if (strncmp(argv[i], "--root=", 7) == 0)
			snprintf(g_conf.root, sizeof(g_conf.root), "%s", argv[i] + 7);
		else if (strncmp(argv[i], "--bind=", 7) == 0)
			snprintf(g_conf.bind, sizeof(g_conf.bind), "%s", argv[i] + 7);
		else if (strncmp(argv[i], "--port=", 7) == 0)
			g_conf.port = atoi(argv[i] + 7);
		else if (strncmp(argv[i], "--web-root=", 11) == 0)
			snprintf(g_conf.web_root, sizeof(g_conf.web_root), "%s", argv[i] + 11);
		else if (strncmp(argv[i], "--push-token=", 13) == 0)
			snprintf(g_conf.push_token, sizeof(g_conf.push_token), "%s", argv[i] + 13);
		else if (strncmp(argv[i], "--pull-token=", 13) == 0)
			snprintf(g_conf.pull_token, sizeof(g_conf.pull_token), "%s", argv[i] + 13);
		else if (strcmp(argv[i], "--import") == 0)
			do_import = 1;
		else if (strcmp(argv[i], "--canonicalize") == 0)
			do_canonicalize = 1;
		else if (strncmp(argv[i], "--set-arch=", 11) == 0)
			set_arch = argv[i] + 11;
		else if (strcmp(argv[i], "--dry-run") == 0)
			dry_run = 1;
		else if (strcmp(argv[i], "--version") == 0) {
			printf("cixcached %s (%s)\n", CIXCACHE_BUILD_VERSION, CIXCACHE_BUILD_TIME);
			return 0;
		} else if (strcmp(argv[i], "--help") == 0) {
			usage(stdout);
			return 0;
		} else {
			fprintf(stderr, "cixcached: unknown option '%s'\n", argv[i]);
			usage(stderr);
			return 2;
		}
	}

	if (store_init(g_conf.root) != 0)
		return 1;

	if (do_import) {
		char manifest[PATH_MAX];

		snprintf(manifest, sizeof(manifest), "%s/MANIFEST.json", g_conf.root);
		return importer_run(g_conf.root, manifest, dry_run, NULL) == 0 ? 0 : 1;
	}

	/*
	 * Offline and explicit, like --import: a store migration is not
	 * something a daemon should decide to do to an operator's data on
	 * its way up. It has to be run once after upgrading past v2.3.0,
	 * because from then on the server looks for canonical names --
	 * an unmigrated entry stays listed but stops resolving.
	 */
	if (do_canonicalize) {
		int renamed = 0;
		int conflicts = 0;
		int rc = store_canonicalize(dry_run, &renamed, &conflicts);

		printf("%s %d entries, %d conflicts\n", dry_run ? "would rename" : "renamed", renamed,
		       conflicts);
		return rc == 0 && conflicts == 0 ? 0 : 1;
	}

	/*
	 * An assertion an operator makes, never an inference: it says
	 * "everything already in this store was built for ARCH". Nothing
	 * else in the server will ever add an architecture to a name.
	 */
	if (set_arch != NULL) {
		int renamed = 0;
		int conflicts = 0;
		int rc = store_set_arch(set_arch, dry_run, &renamed, &conflicts);

		printf("%s %d entries, %d conflicts\n", dry_run ? "would stamp" : "stamped", renamed,
		       conflicts);
		return rc == 0 && conflicts == 0 ? 0 : 1;
	}

	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	g_epfd = epoll_create1(EPOLL_CLOEXEC);
	if (g_epfd < 0) {
		perror("epoll_create1");
		return 1;
	}
	listen_fd = create_listener(g_conf.bind, g_conf.port);
	if (listen_fd < 0)
		return 1;
	listener = conn_new(CONN_LISTENER, listen_fd);
	if (listener == NULL || epoll_add(listener, EPOLLIN) != 0) {
		fprintf(stderr, "cixcached: cannot register listener\n");
		return 1;
	}
	g_started_ms = now_ms();
	server_log("info", "cixcached %s serving %s on %s:%d", CIXCACHE_BUILD_VERSION, g_conf.root,
	           g_conf.bind, g_conf.port);
	fflush(stdout);

	while (!g_stop) {
		struct cix_epoll_event events[MAX_EVENTS];
		int n;
		int k;

		/*
		 * A one-second tick even when idle, so stalled transfers are
		 * swept on schedule rather than only when other traffic
		 * happens to wake the loop.
		 */
		n = cix_epoll_wait(g_epfd, events, MAX_EVENTS, 1000);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("epoll_wait");
			break;
		}
		for (k = 0; k < n; k++) {
			struct conn *cc = events[k].data.ptr;

			if (cc == NULL || cc->kind == CONN_DEAD)
				continue;
			if (cc->kind == CONN_LISTENER) {
				accept_loop(cc);
				continue;
			}
			if (cc->kind == CONN_HASH_CHILD) {
				struct conn *owner = cc->owner;

				cc->owner = NULL;
				if (owner != NULL) {
					owner->child = NULL;
					conn_close(cc);
					finish_upload(owner);
				} else {
					conn_close(cc);
				}
				continue;
			}
			if (cc->kind == CONN_IMPORT_CHILD) {
				int status = 0;

				if (cc->child_pid > 0)
					waitpid(cc->child_pid, &status, 0);
				g_import_running = 0;
				g_import_last_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
				conn_close(cc);
				continue;
			}
			if ((events[k].events & (EPOLLHUP | EPOLLERR)) != 0) {
				conn_close(cc);
				continue;
			}
			if ((events[k].events & EPOLLIN) != 0)
				handle_read(cc);
			if (cc->kind != CONN_DEAD && (events[k].events & EPOLLOUT) != 0)
				handle_write(cc);
		}
		sweep_timeouts();
		drain_pending_free();
	}
	printf("cixcached: shutting down\n");
	return 0;
}
