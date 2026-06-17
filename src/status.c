/*
 * omdfs — sync-status surfacing (implementation).
 *
 * Writes a small key: value text file at DATADIR/state/status. The write is
 * atomic (write to status.tmp, fsync, rename over status) so a reader never sees
 * a half-written file. There is no in-memory state beyond the start time; the
 * syncer hands us a fully-populated snapshot each time.
 */
#define _GNU_SOURCE
#include "status.h"

#include "omdfs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static time_t g_started; /* daemon start (for an uptime-ish "started" line) */

static void status_path(char out[PATH_MAX])
{
	snprintf(out, PATH_MAX, "%s/state/status", g_cfg.datadir);
}

/* Format an epoch as ISO-8601 UTC ("2026-06-15T12:34:56Z"); "-" if epoch <= 0. */
static void fmt_time(long long epoch, char out[32])
{
	if (epoch <= 0) {
		snprintf(out, 32, "-");
		return;
	}
	time_t t = (time_t)epoch;
	struct tm tm;
	gmtime_r(&t, &tm);
	strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* One-word overall state for a quick glance. */
static const char *overall(const struct omdfs_status *s)
{
	if (s->stuck)
		return "stuck";
	if (s->backpressure)
		return "backpressure";
	if (s->pending_structural || s->dirty_files)
		return "syncing";
	return "ok";
}

static int write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	while (len) {
		ssize_t n = write(fd, p, len);
		if (n <= 0)
			return -1;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

void status_publish(const struct omdfs_status *s)
{
	char now_s[32], last_s[32], started_s[32], resync_s[32], mark_s[32];
	long long now = (long long)time(NULL);
	fmt_time(now, now_s);
	fmt_time(s->last_sync_epoch, last_s);
	fmt_time((long long)g_started, started_s);
	fmt_time(s->last_resync_epoch, resync_s);
	const char *resync_res = (s->last_resync[0]) ? s->last_resync : "-";
	fmt_time(s->last_mark_epoch, mark_s);
	const char *mark_res = (s->last_mark[0]) ? s->last_mark : "-";

	const char *stuck_op = (s->stuck_op[0]) ? s->stuck_op : "-";
	const char *stuck_path = (s->stuck_path[0]) ? s->stuck_path : "-";
	char errbuf[64];
	if (s->stuck_errno)
		snprintf(errbuf, sizeof(errbuf), "%d (%s)", s->stuck_errno,
			 strerror(s->stuck_errno));
	else
		snprintf(errbuf, sizeof(errbuf), "-");

	char body[2 * PATH_MAX + 1024];
	int len = snprintf(body, sizeof(body),
		"omdfs status\n"
		"state: %s\n"
		"generated: %s\n"
		"started: %s\n"
		"last-clean-sync: %s\n"
		"pending-structural: %ld\n"
		"dirty-files: %ld\n"
		"dirty-bytes: %lld\n"
		"cache-bytes: %lld\n"
		"cache-budget: %lld\n"
		"cache-hard-limit: %lld\n"
		"backpressure: %s\n"
		"stuck: %s\n"
		"stuck-op: %s\n"
		"stuck-path: %s\n"
		"stuck-errno: %s\n"
		"last-resync: %s\n"
		"last-resync-result: %s\n"
		"last-mark-dirty: %s\n"
		"last-mark-dirty-result: %s\n",
		overall(s), now_s, started_s, last_s,
		s->pending_structural, s->dirty_files, s->dirty_bytes,
		s->cache_bytes, s->cache_budget, s->cache_hard_limit,
		s->backpressure ? "yes" : "no",
		s->stuck ? "yes" : "no",
		stuck_op, stuck_path, errbuf, resync_s, resync_res,
		mark_s, mark_res);
	if (len < 0)
		return;
	if (len > (int)sizeof(body))
		len = (int)sizeof(body);

	char sp[PATH_MAX], tmp[PATH_MAX];
	status_path(sp);
	snprintf(tmp, sizeof(tmp), "%s.tmp", sp);

	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return;
	if (write_all(fd, body, (size_t)len) != 0 || fsync(fd) != 0) {
		close(fd);
		unlink(tmp);
		return;
	}
	close(fd);
	if (rename(tmp, sp) != 0)
		unlink(tmp);
}

void status_init(void)
{
	g_started = time(NULL);
	struct omdfs_status s;
	memset(&s, 0, sizeof(s));
	status_publish(&s); /* "ok" / empty backlog until the first sync cycle */
}
