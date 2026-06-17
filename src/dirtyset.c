/*
 * omdfs — in-memory dirty-directory set (implementation).
 *
 * A chained hash table of owned path strings, guarded by one mutex. Membership
 * is a small set (only directories with pending writes), so a fixed bucket count
 * with chaining is plenty; operations are short and never touch I/O, so the lock
 * is never held across a syscall. `g_force_full` starts set so the syncer's first
 * cycle does a full scan (seeding the set on a fresh or crash-recovered mount);
 * it is re-armed whenever an allocation failure could have dropped a member, so
 * correctness degrades to "scan everything once" rather than "miss a flush".
 */
#define _GNU_SOURCE
#include "dirtyset.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define DS_BUCKETS 1024u

struct ds_node {
	char *path;
	struct ds_node *hnext;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct ds_node *g_buckets[DS_BUCKETS];
static size_t g_count;
static int g_force_full = 1; /* first take -> full recovery scan */

/* djb2-ish over the path (matches lru.c's style). */
static unsigned hash_path(const char *s)
{
	unsigned h = 5381;
	for (; *s; s++)
		h = ((h << 5) + h) ^ (unsigned char)*s;
	return h & (DS_BUCKETS - 1);
}

/* g_lock held. Insert `path`, taking ownership. If a key already matches, or the
 * node can't be allocated, free `path` (and arm a full scan on failure). */
static void insert_owned(char *path)
{
	unsigned b = hash_path(path);
	for (struct ds_node *n = g_buckets[b]; n; n = n->hnext) {
		if (!strcmp(n->path, path)) {
			free(path);
			return;
		}
	}
	struct ds_node *n = malloc(sizeof(*n));
	if (!n) {
		free(path);
		g_force_full = 1; /* dropped a member: fall back to a full scan */
		return;
	}
	n->path = path;
	n->hnext = g_buckets[b];
	g_buckets[b] = n;
	g_count++;
}

void dirtyset_add(const char *fuse_dir)
{
	char *p = strdup(fuse_dir);
	pthread_mutex_lock(&g_lock);
	if (!p)
		g_force_full = 1;
	else
		insert_owned(p);
	pthread_mutex_unlock(&g_lock);
}

void dirtyset_force_full(void)
{
	pthread_mutex_lock(&g_lock);
	g_force_full = 1;
	pthread_mutex_unlock(&g_lock);
}

void dirtyset_rename(const char *from, const char *to)
{
	size_t flen = strlen(from), tlen = strlen(to);
	pthread_mutex_lock(&g_lock);

	/* Detach every key at or under `from` (rewriting changes the hash, so it
	 * can't be done in place), then re-insert each under `to`. */
	struct ds_node *moved = NULL;
	for (unsigned b = 0; b < DS_BUCKETS; b++) {
		struct ds_node **pp = &g_buckets[b];
		while (*pp) {
			struct ds_node *n = *pp;
			const char *p = n->path;
			if (!strcmp(p, from) ||
			    (!strncmp(p, from, flen) && p[flen] == '/')) {
				*pp = n->hnext; /* detach */
				g_count--;
				n->hnext = moved;
				moved = n;
			} else {
				pp = &n->hnext;
			}
		}
	}
	while (moved) {
		struct ds_node *n = moved;
		moved = n->hnext;
		size_t tail = strlen(n->path) - flen; /* suffix after `from` */
		char *np = malloc(tlen + tail + 1);
		if (np) {
			memcpy(np, to, tlen);
			memcpy(np + tlen, n->path + flen, tail + 1); /* incl NUL */
			insert_owned(np);
		} else {
			g_force_full = 1;
		}
		free(n->path);
		free(n);
	}
	pthread_mutex_unlock(&g_lock);
}

char **dirtyset_take(size_t *n_out, int *full)
{
	pthread_mutex_lock(&g_lock);
	*full = g_force_full;
	g_force_full = 0;

	size_t n = g_count;
	char **arr = n ? malloc(n * sizeof(*arr)) : NULL;
	if (n && !arr) {
		/* Can't extract: leave the set intact and force a full scan. */
		g_force_full = 1;
		*full = 1;
		pthread_mutex_unlock(&g_lock);
		*n_out = 0;
		return NULL;
	}

	size_t i = 0;
	for (unsigned b = 0; b < DS_BUCKETS; b++) {
		struct ds_node *node = g_buckets[b];
		while (node) {
			struct ds_node *next = node->hnext;
			arr[i++] = node->path; /* transfer ownership */
			free(node);
			node = next;
		}
		g_buckets[b] = NULL;
	}
	g_count = 0;
	pthread_mutex_unlock(&g_lock);

	*n_out = i;
	return arr;
}

void dirtyset_free(char **paths, size_t n)
{
	if (!paths)
		return;
	for (size_t i = 0; i < n; i++)
		free(paths[i]);
	free(paths);
}
