/*
 * cixcachectl: a pure REST client for the cix-cache artifact registry.
 *
 * Follows the same rule cixctl follows for the Cix host API (that
 * project's ADR-0005): this file holds no store logic of its own --
 * every subcommand is exactly one HTTP call. The server owns the
 * store, so operations like import and gc are triggered here and
 * performed there, rather than reimplemented against the filesystem
 * behind the server's back.
 */
#include "httpclient.h"
#include "json.h"

#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * List output goes through the user's pager when stdout is a terminal.
 * 85 artifacts do not fit on a screen, and a registry only gets more
 * of them. Redirected or piped output is left completely alone.
 */
static FILE *g_out;

static void out_open(int paged)
{
	const char *pager;

	g_out = stdout;
	if (!paged || !isatty(STDOUT_FILENO))
		return;
	pager = getenv("PAGER");
	if (pager == NULL || pager[0] == '\0')
		pager = "less -FRX";
	{
		FILE *p = popen(pager, "w");

		if (p != NULL)
			g_out = p;
	}
}

static void out_close(void)
{
	if (g_out != NULL && g_out != stdout)
		pclose(g_out);
	g_out = stdout;
}

/* Bytes a person can read at a glance, not a number they have to count. */
static void human_bytes(long long n, char *out, size_t out_size)
{
	static const char *unit[] = { "B", "K", "M", "G", "T" };
	double v = (double)n;
	int i = 0;

	while (v >= 1024.0 && i < 4) {
		v /= 1024.0;
		i++;
	}
	if (i == 0)
		snprintf(out, out_size, "%lld%s", n, unit[i]);
	else
		snprintf(out, out_size, "%.1f%s", v, unit[i]);
}

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 8080
#define UPLOAD_BUF 65536

static const char *str_field(const struct json_value *v, const char *key)
{
	const char *s = json_as_string(json_object_get(v, key));

	return s != NULL ? s : "";
}

static long long int_field(const struct json_value *v, const char *key)
{
	return (long long)json_as_number(json_object_get(v, key));
}

/*
 * json_as_number() only speaks JSON_NUMBER, so a bool read through it
 * always comes back 0 -- which silently turns every "is this open?"
 * question into "no". Read the boolean arm directly instead.
 */
static int bool_field(const struct json_value *v, const char *key)
{
	const struct json_value *f = json_object_get(v, key);

	return f != NULL && f->type == JSON_BOOL && f->u.boolean != 0;
}

static void print_raw_json(const struct json_value *v)
{
	struct json_writer w;

	if (v == NULL)
		return;
	jw_init(&w);
	jw_value(&w, v);
	fwrite(w.buf, 1, w.len, stdout);
	printf("\n");
	jw_free(&w);
}

/*
 * Common success/failure handling for every subcommand: 2xx prints
 * either the raw JSON (--json, or when a subcommand has no special
 * formatting) or a formatted rendering via fmt; anything else prints
 * the API's {"error": "..."} to stderr. Always frees r. Returns the
 * process exit code.
 */
static int emit(struct cix_response *r, int json_mode, void (*fmt)(const struct json_value *))
{
	int rc;

	if (r->status < 200 || r->status >= 300) {
		const char *msg = json_as_string(json_object_get(r->json, "error"));

		fprintf(stderr, "cixcachectl: %s (HTTP %d)\n", msg != NULL ? msg : "request failed",
		        r->status);
		rc = 1;
	} else if (json_mode || fmt == NULL) {
		print_raw_json(r->json);
		rc = 0;
	} else {
		fmt(r->json);
		rc = 0;
	}
	cix_response_free(r);
	return rc;
}

static void fmt_status(const struct json_value *v)
{
	fprintf(g_out, "build:     %s (%s)\n", str_field(v, "build_version"), str_field(v, "build_time"));
	fprintf(g_out, "root:      %s\n", str_field(v, "root"));
	fprintf(g_out, "uptime:    %llds\n", int_field(v, "uptime_seconds"));
	fprintf(g_out, "requests:  %lld\n", int_field(v, "requests"));
	/*
	 * Hits and misses separately, because a registry serving the wrong
	 * paths looks identical to an empty one from the outside: every
	 * request 404s and every host silently rebuilds from source.
	 */
	fprintf(g_out, "lookups:   %lld hit, %lld missed\n", int_field(v, "artifact_hits"),
	        int_field(v, "artifact_misses"));
	/*
	 * Hits served under a non-canonical name. They worked -- the two
	 * names are one entry -- so nothing else would ever mention them,
	 * which is exactly why a count belongs here: it is the only sign
	 * that a recipe somewhere still spells a name the old way.
	 */
	if (int_field(v, "artifact_aliases") > 0)
		fprintf(g_out, "aliases:   %lld served under a non-canonical name\n",
		        int_field(v, "artifact_aliases"));
	fprintf(g_out, "served:    %lld bytes\n", int_field(v, "served_bytes"));
	/*
	 * Two different facts: this store holds four zlibs and three
	 * greps, so the number of packages and the number of published
	 * files are not the same question.
	 */
	fprintf(g_out, "packages:  %lld unique, %lld artifacts (%lld bytes)\n",
	        int_field(v, "unique_packages"), int_field(v, "packages"),
	        int_field(v, "package_bytes"));
	/*
	 * Counted apart from packages, mirroring MANIFEST.json's two
	 * sections, and shown only when there are any -- a line that always
	 * reads zero is noise.
	 */
	if (int_field(v, "installers") > 0)
		fprintf(g_out, "installers: %lld (%lld bytes, signatures included)\n",
		        int_field(v, "installers"), int_field(v, "installer_bytes"));
	/*
	 * Only when there are any. An artifact with no architecture serves
	 * correctly until the day a build for another machine shares its
	 * name, so nothing else will ever mention it.
	 */
	if (int_field(v, "unstamped") > 0)
		fprintf(g_out, "unstamped: %lld with no architecture -- run --set-arch\n",
		        int_field(v, "unstamped"));
	fprintf(g_out, "pull:      %s\n", bool_field(v, "pull_open") ? "open" : "token required");
	fprintf(g_out, "push:      %s\n", bool_field(v, "push_configured") ? "token required" : "OPEN");
}

/*
 * Whether the architecture column is worth its width. Same rule as the
 * dashboard: a column repeating one value teaches nothing, so it shows
 * up only once two artifacts disagree.
 */
static int g_show_arch;

/*
 * Likewise for the signature. Only bootables carry one, so the column
 * appears only when something in the listing actually does -- against a
 * store of packages it would be a column of blanks.
 */
static int g_show_signed;

static void fmt_artifact_line(const struct json_value *v)
{
	const char *version = str_field(v, "version");
	time_t modified = (time_t)int_field(v, "modified");
	char when[16] = "-";
	char size[16];
	struct tm tm;

	human_bytes(int_field(v, "bytes"), size, sizeof(size));
	if (modified > 0) {
		localtime_r(&modified, &tm);
		strftime(when, sizeof(when), "%Y-%m-%d", &tm);
	}
	/*
	 * Name, version and release in their own columns, and no URL
	 * column: the URL is the name, at the root of base_url. Printing
	 * both just makes the line too wide to read.
	 *
	 * The release is its own column rather than part of the version
	 * because it is Cix's number, not upstream's -- 5.2.37 is what
	 * the bash authors released, -2 is what we did to it.
	 */
	{
		const char *arch = str_field(v, "arch");
		char cols[64];
		int n = 0;

		cols[0] = '\0';
		if (g_show_arch)
			n += snprintf(cols + n, sizeof(cols) - (size_t)n, "%-8s ",
			              arch[0] != '\0' ? arch : "-");
		/*
		 * Blank rather than "no" for an artifact that carries no
		 * signature at all: a package is not unsigned, signing is
		 * simply not a thing it does.
		 */
		if (g_show_signed) {
			const struct json_value *sig = json_object_get(v, "signed");

			n += snprintf(cols + n, sizeof(cols) - (size_t)n, "%-6s ",
			              sig == NULL ? "" : (bool_field(v, "signed") ? "yes" : "no"));
		}
		(void)n;
		fprintf(g_out, "%-18s %12.12s %4lld %s %9s  %.12s  %10s\n", str_field(v, "artifact"),
		        version[0] != '\0' ? version : "-", int_field(v, "release"), cols, size,
		        str_field(v, "sha256"), when);
	}
}

static void fmt_artifacts(const struct json_value *v)
{
	const struct json_value *arr = json_object_get(v, "artifacts");
	char total[16];
	size_t i;

	if (arr == NULL || arr->type != JSON_ARRAY)
		return;
	g_show_arch = 0;
	g_show_signed = 0;
	for (i = 0; i < arr->u.array.count; i++) {
		if (i > 0 && strcmp(str_field(arr->u.array.items[i], "arch"),
		                    str_field(arr->u.array.items[0], "arch")) != 0)
			g_show_arch = 1;
		if (json_object_get(arr->u.array.items[i], "signed") != NULL)
			g_show_signed = 1;
	}
	{
		char head[64];
		int n = 0;

		head[0] = '\0';
		if (g_show_arch)
			n += snprintf(head + n, sizeof(head) - (size_t)n, "%-8s ", "ARCH");
		if (g_show_signed)
			n += snprintf(head + n, sizeof(head) - (size_t)n, "%-6s ", "SIGNED");
		(void)n;
		fprintf(g_out, "%-18s %12s %4s %s %9s  %-12s  %10s\n", "ARTIFACT", "VERSION", "REL",
		        head, "SIZE", "SHA256", "PUBLISHED");
	}
	for (i = 0; i < arr->u.array.count; i++)
		fmt_artifact_line(arr->u.array.items[i]);
	human_bytes(int_field(v, "bytes"), total, sizeof(total));
	/*
	 * Broken down when both kinds are present. Otherwise this total
	 * and the one in `status` use the word "artifacts" for different
	 * sets -- status counts packages and installers separately, this
	 * listing shows both -- and the two numbers appear to disagree.
	 */
	if (g_show_signed) {
		long long installers = 0;

		for (i = 0; i < arr->u.array.count; i++) {
			if (json_object_get(arr->u.array.items[i], "signed") != NULL)
				installers++;
		}
		fprintf(g_out, "\n%lld artifacts (%lld packages, %lld installers), %s\n",
		        int_field(v, "count"), int_field(v, "count") - installers, installers, total);
		return;
	}
	fprintf(g_out, "\n%lld artifacts, %s\n", int_field(v, "count"), total);
}

static void fmt_log(const struct json_value *v)
{
	const struct json_value *arr = json_object_get(v, "entries");
	size_t i;

	if (arr == NULL || arr->type != JSON_ARRAY)
		return;
	for (i = 0; i < arr->u.array.count; i++) {
		const struct json_value *e = arr->u.array.items[i];
		time_t secs = (time_t)(int_field(e, "time_ms") / 1000);
		struct tm tm;
		char when[32];

		localtime_r(&secs, &tm);
		strftime(when, sizeof(when), "%H:%M:%S", &tm);
		fprintf(g_out, "%s  %-5s %s\n", when, str_field(e, "level"), str_field(e, "text"));
	}
}

static void fmt_gc(const struct json_value *v)
{
	fprintf(g_out, "%s: %lld blobs, %lld bytes\n", bool_field(v, "dry_run") ? "would remove" : "removed",
	       int_field(v, "removed"), int_field(v, "bytes_freed"));
}

static void fmt_import_status(const struct json_value *v)
{
	fprintf(g_out, "running:   %s\n", bool_field(v, "running") ? "yes" : "no");
	fprintf(g_out, "elapsed:   %llds\n", int_field(v, "elapsed_seconds"));
	fprintf(g_out, "last exit: %lld\n", int_field(v, "last_exit_status"));
}

static void fmt_accepted(const struct json_value *v)
{
	(void)v;
	fprintf(g_out, "accepted\n");
}

static void fmt_removed(const struct json_value *v)
{
	(void)v;
	fprintf(g_out, "removed\n");
}

/*
 * PUT is the one subcommand that cannot go through
 * cix_client_request(): that takes the body as a NUL-terminated string
 * held whole in memory, and an image artifact here is 2.7 GB. The
 * request is written by hand onto a raw socket and the file streamed
 * through a fixed buffer instead. Still exactly one HTTP call.
 */
static int cmd_put(const struct cix_client *c, int argc, char **argv)
{
	char buf[UPLOAD_BUF];
	const char *path = NULL;
	const char *name = NULL;
	const char *token = NULL;
	const char *digest = NULL;
	char url[600];
	char req[1024];
	struct stat st;
	long long sent = 0;
	int fd;
	int sock;
	int status = 0;
	int i;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--sha256=", 9) == 0)
			digest = argv[i] + 9;
		else if (strncmp(argv[i], "--token=", 8) == 0)
			token = argv[i] + 8;
		else if (strncmp(argv[i], "--", 2) == 0) {
			fprintf(stderr, "cixcachectl: unknown put option '%s'\n", argv[i]);
			return 2;
		} else if (path == NULL)
			path = argv[i];
		else {
			fprintf(stderr, "cixcachectl: unexpected argument '%s'\n", argv[i]);
			return 2;
		}
	}
	if (path == NULL || name == NULL || digest == NULL) {
		fprintf(stderr,
		        "usage: cixcachectl put FILE --name=NAME.tar.gz --sha256=HEX [--token=TOK]\n");
		return 2;
	}
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "cixcachectl: %s: %s\n", path, strerror(errno));
		return 1;
	}
	if (fstat(fd, &st) != 0) {
		close(fd);
		fprintf(stderr, "cixcachectl: cannot stat %s\n", path);
		return 1;
	}
	snprintf(url, sizeof(url), "/%s", name);
	sock = cix_client_connect_raw(c);
	if (sock < 0) {
		close(fd);
		fprintf(stderr, "cixcachectl: could not reach server\n");
		return 1;
	}
	snprintf(req, sizeof(req),
	         "PUT %s HTTP/1.1\r\nHost: %s\r\nContent-Length: %lld\r\n"
	         "X-Cix-Sha256: %s\r\n%s%s%sConnection: close\r\n\r\n",
	         url, c->host, (long long)st.st_size, digest, token != NULL ? "Authorization: Bearer " : "",
	         token != NULL ? token : "", token != NULL ? "\r\n" : "");
	if (write(sock, req, strlen(req)) != (ssize_t)strlen(req)) {
		close(fd);
		close(sock);
		fprintf(stderr, "cixcachectl: short write sending request\n");
		return 1;
	}
	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));
		ssize_t off = 0;

		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0)
			break;
		while (off < n) {
			ssize_t w = write(sock, buf + off, (size_t)(n - off));

			if (w < 0) {
				if (errno == EINTR)
					continue;
				fprintf(stderr, "cixcachectl: connection closed after %lld bytes\n", sent);
				close(fd);
				close(sock);
				return 1;
			}
			off += w;
			sent += w;
		}
	}
	close(fd);
	{
		char resp[4096];
		ssize_t n = read(sock, resp, sizeof(resp) - 1);

		if (n > 0) {
			resp[n] = '\0';
			if (sscanf(resp, "HTTP/1.%*d %d", &status) != 1)
				status = 0;
		}
	}
	close(sock);
	if (status >= 200 && status < 300) {
		printf("published %s (%lld bytes)\n", name, sent);
		return 0;
	}
	fprintf(stderr, "cixcachectl: push rejected (HTTP %d)\n", status);
	return 1;
}

static int one_call(const struct cix_client *c, int json_mode, const char *method, const char *path,
                    const char *token, void (*fmt)(const struct json_value *), int paged)
{
	struct cix_response r;
	int rc;

	if (cix_client_request_with_auth(c, method, path, token, NULL, &r) != 0) {
		fprintf(stderr, "cixcachectl: could not reach server\n");
		return 1;
	}
	out_open(paged && !json_mode);
	rc = emit(&r, json_mode, fmt);
	out_close();
	return rc;
}

/*
 * Tails the server's activity ring, asking only for what it has not
 * already shown. One small response per second rather than the whole
 * ring each time.
 */
static int cmd_log_follow(const struct cix_client *c, const char *token)
{
	long long after = 0;

	g_out = stdout;
	for (;;) {
		struct cix_response r;
		char path[64];

		snprintf(path, sizeof(path), "/api/v1/log?after=%lld", after);
		if (cix_client_request_with_auth(c, "GET", path, token, NULL, &r) != 0) {
			fprintf(stderr, "cixcachectl: could not reach server\n");
			return 1;
		}
		if (r.status < 200 || r.status >= 300) {
			cix_response_free(&r);
			fprintf(stderr, "cixcachectl: log request failed (HTTP %d)\n", r.status);
			return 1;
		}
		fmt_log(r.json);
		fflush(stdout);
		after = (long long)json_as_number(json_object_get(r.json, "seq"));
		cix_response_free(&r);
		sleep(1);
	}
}

static void usage(FILE *out)
{
	fprintf(out,
	        "usage: cixcachectl [--host=H] [--port=N] [--json] <command>\n"
	        "\n"
	        "  status                     server and store summary\n"
	        "  ls                         every published artifact\n"
	        "  manifest                   MANIFEST.json, generated live\n"
	        "  log [-f]                   what the server has been doing\n"
	        "  gc [--dry-run] [--token=]  remove blobs no name points at\n"
	        "  import [--token=]          migrate a static export into the store\n"
	        "  import-status              progress of a running import\n"
	        "  put FILE --name=N --sha256=H [--token=]\n"
	        "  rm NAME [--token=]\n");
}

static int dispatch_command(const struct cix_client *c, int json_mode, const char *cmd, int argc,
                            char **argv)
{
	const char *token = NULL;
	int dry_run = 0;
	int follow = 0;
	int i;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--token=", 8) == 0)
			token = argv[i] + 8;
		else if (strcmp(argv[i], "--dry-run") == 0)
			dry_run = 1;
		else if (strcmp(argv[i], "--follow") == 0 || strcmp(argv[i], "-f") == 0)
			follow = 1;
	}
	if (strcmp(cmd, "status") == 0)
		return one_call(c, json_mode, "GET", "/api/v1/status", token, fmt_status, 0);
	if (strcmp(cmd, "ls") == 0)
		return one_call(c, json_mode, "GET", "/api/v1/artifacts", token, fmt_artifacts, 1);
	if (strcmp(cmd, "manifest") == 0)
		return one_call(c, json_mode, "GET", "/MANIFEST.json", token, NULL, 1);
	if (strcmp(cmd, "log") == 0) {
		if (follow)
			return cmd_log_follow(c, token);
		return one_call(c, json_mode, "GET", "/api/v1/log?after=0", token, fmt_log, 1);
	}
	if (strcmp(cmd, "gc") == 0)
		return one_call(c, json_mode, dry_run ? "GET" : "POST", "/api/v1/gc", token, fmt_gc, 0);
	if (strcmp(cmd, "import") == 0)
		return one_call(c, json_mode, "POST", "/api/v1/import", token, fmt_accepted, 0);
	if (strcmp(cmd, "import-status") == 0)
		return one_call(c, json_mode, "GET", "/api/v1/import-status", token, fmt_import_status,
		                0);
	if (strcmp(cmd, "put") == 0)
		return cmd_put(c, argc, argv);
	if (strcmp(cmd, "rm") == 0) {
		char path[600];

		if (argc < 1 || argv[0][0] == '-') {
			fprintf(stderr, "usage: cixcachectl rm NAME [--token=TOK]\n");
			return 2;
		}
		snprintf(path, sizeof(path), "/%s", argv[0]);
		return one_call(c, json_mode, "DELETE", path, token, fmt_removed, 0);
	}
	fprintf(stderr, "cixcachectl: unknown command '%s'\n", cmd);
	usage(stderr);
	return 2;
}

int main(int argc, char **argv)
{
	const char *host = DEFAULT_HOST;
	struct cix_client client;
	int port = DEFAULT_PORT;
	int json_mode = 0;
	const char *cmd;
	int i = 1;

	while (i < argc && strncmp(argv[i], "--", 2) == 0) {
		if (strncmp(argv[i], "--host=", 7) == 0)
			host = argv[i] + 7;
		else if (strncmp(argv[i], "--port=", 7) == 0)
			port = atoi(argv[i] + 7);
		else if (strcmp(argv[i], "--json") == 0)
			json_mode = 1;
		else if (strcmp(argv[i], "--help") == 0) {
			usage(stdout);
			return 0;
		} else
			break;
		i++;
	}
	g_out = stdout;
	cix_client_init(&client, host, port);
	if (i >= argc) {
		usage(stderr);
		return 2;
	}
	cmd = argv[i++];
	return dispatch_command(&client, json_mode, cmd, argc - i, argv + i);
}
