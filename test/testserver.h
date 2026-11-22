#ifndef TESTSERVER_H
#define TESTSERVER_H

/*
 * Shared harness for the integration tests: start a real cixcached on
 * a dedicated port over a throwaway store, drive it with real HTTP,
 * kill it. No mocks and no framework, matching how the Cix project
 * tests its own daemon.
 *
 * Included by exactly one translation unit per test binary, which is
 * why these can be static inline -- TCC emits a strong symbol for
 * every static inline in every TU, so a header like this shared across
 * many objects in one link would collide.
 */
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct testserver {
	pid_t pid;
	int port;
	char root[256];
};

static int ts_port_open(int port)
{
	struct sockaddr_in addr;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	int ok;

	if (fd < 0)
		return 0;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)port);
	addr.sin_addr.s_addr = htonl(0x7f000001);
	ok = connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
	close(fd);
	return ok;
}

/*
 * Starts cixcached with the harness's own flags plus extra, a
 * NULL-terminated list (or NULL), and waits until wait_port accepts a
 * connection. Returns 0, or -1 if it never did. A server that exits
 * instead -- one that refused its configuration -- is noticed at once
 * rather than waited out.
 */
static int ts_start_with(struct testserver *ts, int port, const char *push_token,
                         const char *const *extra, int wait_port)
{
	char tmpl[] = "/tmp/cixcache-it-XXXXXX";
	char portarg[32];
	char rootarg[300];
	char tokarg[300];
	int tries;

	if (mkdtemp(tmpl) == NULL)
		return -1;
	snprintf(ts->root, sizeof(ts->root), "%s", tmpl);
	ts->port = port;
	snprintf(portarg, sizeof(portarg), "--port=%d", port);
	snprintf(rootarg, sizeof(rootarg), "--root=%s", ts->root);
	snprintf(tokarg, sizeof(tokarg), "--push-token=%s", push_token != NULL ? push_token : "");

	ts->pid = fork();
	if (ts->pid < 0)
		return -1;
	if (ts->pid == 0) {
		const char *args[32];
		int devnull = open("/dev/null", O_WRONLY);
		int n = 0;

		if (devnull >= 0) {
			dup2(devnull, STDOUT_FILENO);
			close(devnull);
		}
		args[n++] = "cixcached";
		args[n++] = rootarg;
		args[n++] = portarg;
		args[n++] = "--bind=127.0.0.1";
		args[n++] = tokarg;
		while (extra != NULL && *extra != NULL && n < 31)
			args[n++] = *extra++;
		args[n] = NULL;
		execv("build/cixcached", (char *const *)args);
		_exit(127);
	}
	for (tries = 0; tries < 200; tries++) {
		if (ts_port_open(wait_port))
			return 0;
		if (waitpid(ts->pid, NULL, WNOHANG) == ts->pid) {
			ts->pid = -1;
			return -1;
		}
		usleep(25000);
	}
	return -1;
}

static int ts_start(struct testserver *ts, int port, const char *push_token)
{
	return ts_start_with(ts, port, push_token, NULL, port);
}

static void ts_stop(struct testserver *ts)
{
	char cmd[400];

	if (ts->pid > 0) {
		kill(ts->pid, SIGTERM);
		waitpid(ts->pid, NULL, 0);
		ts->pid = -1;
	}
	if (ts->root[0] != '\0') {
		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", ts->root);
		if (system(cmd) != 0)
			fprintf(stderr, "warning: could not clean %s\n", ts->root);
	}
}

/* Runs a shell command and captures its first line of stdout. */
static int ts_capture(const char *cmd, char *out, size_t out_size)
{
	FILE *p = popen(cmd, "r");

	out[0] = '\0';
	if (p == NULL)
		return -1;
	if (fgets(out, (int)out_size, p) == NULL) {
		pclose(p);
		return -1;
	}
	out[strcspn(out, "\r\n")] = '\0';
	return pclose(p) == 0 ? 0 : -1;
}

/* HTTP status of a request, via curl so the transport is the real one. */
static int ts_status(int port, const char *method, const char *path, const char *token)
{
	char cmd[1024];
	char out[64];

	snprintf(cmd, sizeof(cmd),
	         "curl -s -o /dev/null -w '%%{http_code}' -X %s %s%s%s 'http://127.0.0.1:%d%s'",
	         method, token != NULL ? "-H 'Authorization: Bearer " : "",
	         token != NULL ? token : "", token != NULL ? "' " : "", port, path);
	if (ts_capture(cmd, out, sizeof(out)) != 0 && out[0] == '\0')
		return -1;
	return atoi(out);
}

static void ts_make_file(const char *path, long long bytes)
{
	char cmd[512];

	snprintf(cmd, sizeof(cmd),
	         "head -c %lld /dev/urandom > '%s'", bytes, path);
	if (system(cmd) != 0)
		fprintf(stderr, "warning: could not create %s\n", path);
}

static int ts_sha256(const char *path, char *out, size_t out_size)
{
	char cmd[512];

	snprintf(cmd, sizeof(cmd), "/usr/bin/sha256sum '%s' | cut -d' ' -f1", path);
	return ts_capture(cmd, out, out_size);
}

#endif /* TESTSERVER_H */
