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
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
	printf("build:     %s (%s)\n", str_field(v, "build_version"), str_field(v, "build_time"));
	printf("root:      %s\n", str_field(v, "root"));
	printf("uptime:    %llds\n", int_field(v, "uptime_seconds"));
	printf("requests:  %lld\n", int_field(v, "requests"));
	/*
	 * Hits and misses separately, because a registry serving the wrong
	 * paths looks identical to an empty one from the outside: every
	 * request 404s and every host silently rebuilds from source.
	 */
	printf("artifacts: %lld hit, %lld missed\n", int_field(v, "artifact_hits"),
	       int_field(v, "artifact_misses"));
	printf("served:    %lld bytes\n", int_field(v, "served_bytes"));
	printf("packages:  %lld (%lld bytes)\n", int_field(v, "packages"), int_field(v, "package_bytes"));
	printf("images:    %lld (%lld bytes)\n", int_field(v, "images"), int_field(v, "image_bytes"));
	printf("pull:      %s\n", bool_field(v, "pull_open") ? "open" : "token required");
	printf("push:      %s\n", bool_field(v, "push_configured") ? "token required" : "OPEN");
}

static void fmt_artifact_line(const struct json_value *v)
{
	printf("%-9s %-58s %-64s %12lld\n", str_field(v, "tier"), str_field(v, "name"),
	       str_field(v, "sha256"), int_field(v, "bytes"));
}

static void fmt_artifacts(const struct json_value *v)
{
	const struct json_value *arr = json_object_get(v, "artifacts");
	size_t i;

	if (arr == NULL || arr->type != JSON_ARRAY)
		return;
	for (i = 0; i < arr->u.array.count; i++)
		fmt_artifact_line(arr->u.array.items[i]);
	printf("%lld artifacts, %lld bytes\n", int_field(v, "count"), int_field(v, "bytes"));
}

static void fmt_gc(const struct json_value *v)
{
	printf("%s: %lld blobs, %lld bytes\n", bool_field(v, "dry_run") ? "would remove" : "removed",
	       int_field(v, "removed"), int_field(v, "bytes_freed"));
}

static void fmt_import_status(const struct json_value *v)
{
	printf("running:   %s\n", bool_field(v, "running") ? "yes" : "no");
	printf("elapsed:   %llds\n", int_field(v, "elapsed_seconds"));
	printf("last exit: %lld\n", int_field(v, "last_exit_status"));
}

static void fmt_accepted(const struct json_value *v)
{
	(void)v;
	printf("accepted\n");
}

static void fmt_removed(const struct json_value *v)
{
	(void)v;
	printf("removed\n");
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
	int images = 0;
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
		else if (strcmp(argv[i], "--images") == 0)
			images = 1;
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
		fprintf(stderr, "usage: cixcachectl put FILE --name=NAME.tar.gz --sha256=HEX\n"
		                "                         [--images] [--token=TOK]\n");
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
	snprintf(url, sizeof(url), "%s%s", images ? "/images/" : "/", name);
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
                    const char *token, void (*fmt)(const struct json_value *))
{
	struct cix_response r;

	if (cix_client_request_with_auth(c, method, path, token, NULL, &r) != 0) {
		fprintf(stderr, "cixcachectl: could not reach server\n");
		return 1;
	}
	return emit(&r, json_mode, fmt);
}

static void usage(FILE *out)
{
	fprintf(out,
	        "usage: cixcachectl [--host=H] [--port=N] [--json] <command>\n"
	        "\n"
	        "  status                     server and store summary\n"
	        "  ls                         every published artifact\n"
	        "  manifest                   MANIFEST.json, generated live\n"
	        "  gc [--dry-run] [--token=]  remove blobs no name points at\n"
	        "  import [--token=]          migrate a static export into the store\n"
	        "  import-status              progress of a running import\n"
	        "  put FILE --name=N --sha256=H [--images] [--token=]\n"
	        "  rm NAME [--images] [--token=]\n");
}

static int dispatch_command(const struct cix_client *c, int json_mode, const char *cmd, int argc,
                            char **argv)
{
	const char *token = NULL;
	int dry_run = 0;
	int images = 0;
	int i;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--token=", 8) == 0)
			token = argv[i] + 8;
		else if (strcmp(argv[i], "--dry-run") == 0)
			dry_run = 1;
		else if (strcmp(argv[i], "--images") == 0)
			images = 1;
	}
	if (strcmp(cmd, "status") == 0)
		return one_call(c, json_mode, "GET", "/api/v1/status", token, fmt_status);
	if (strcmp(cmd, "ls") == 0)
		return one_call(c, json_mode, "GET", "/api/v1/artifacts", token, fmt_artifacts);
	if (strcmp(cmd, "manifest") == 0)
		return one_call(c, json_mode, "GET", "/MANIFEST.json", token, NULL);
	if (strcmp(cmd, "gc") == 0)
		return one_call(c, json_mode, dry_run ? "GET" : "POST", "/api/v1/gc", token, fmt_gc);
	if (strcmp(cmd, "import") == 0)
		return one_call(c, json_mode, "POST", "/api/v1/import", token, fmt_accepted);
	if (strcmp(cmd, "import-status") == 0)
		return one_call(c, json_mode, "GET", "/api/v1/import-status", token, fmt_import_status);
	if (strcmp(cmd, "put") == 0)
		return cmd_put(c, argc, argv);
	if (strcmp(cmd, "rm") == 0) {
		char path[600];

		if (argc < 1 || argv[0][0] == '-') {
			fprintf(stderr, "usage: cixcachectl rm NAME [--images] [--token=TOK]\n");
			return 2;
		}
		snprintf(path, sizeof(path), "%s%s", images ? "/images/" : "/", argv[0]);
		return one_call(c, json_mode, "DELETE", path, token, fmt_removed);
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
	cix_client_init(&client, host, port);
	if (i >= argc) {
		usage(stderr);
		return 2;
	}
	cmd = argv[i++];
	return dispatch_command(&client, json_mode, cmd, argc - i, argv + i);
}
