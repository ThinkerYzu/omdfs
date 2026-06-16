/*
 * omdfs — in-memory LRU index of cached content files (implementation).
 *
 * Each tracked file is a node carrying its FUSE path, accounted size, and pin
 * count. Nodes live in two structures at once: a doubly-linked recency list
 * (head = most-recently-used, tail = least) and a fixed-bucket hash table keyed
 * by path, so lookups stay O(1) under heavy churn. One mutex guards everything;
 * operations are short (no I/O), so the lock is never held across a syscall.
 */
#define _GNU_SOURCE
#include "lru.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LRU_BUCKETS 4096u

struct lru_node {
	char *path;
	off_t size;
	int open; /* pin count: never evicted while > 0 */
	struct lru_node *prev, *next; /* recency list: head = MRU, tail = LRU */
	struct lru_node *hnext;	      /* hash chain */
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct lru_node *g_head, *g_tail; /* MRU ... LRU */
static struct lru_node *g_buckets[LRU_BUCKETS];
static off_t g_total;

/* djb2 over the path. */
static unsigned hash_path(const char *s)
{
	unsigned h = 5381;
	for (; *s; s++)
		h = ((h << 5) + h) ^ (unsigned char)*s;
	return h & (LRU_BUCKETS - 1);
}

/* g_lock held. */
static struct lru_node *find(const char *path)
{
	for (struct lru_node *n = g_buckets[hash_path(path)]; n; n = n->hnext)
		if (!strcmp(n->path, path))
			return n;
	return NULL;
}

/* g_lock held. Unlink from the recency list. */
static void list_unlink(struct lru_node *n)
{
	if (n->prev)
		n->prev->next = n->next;
	else
		g_head = n->next;
	if (n->next)
		n->next->prev = n->prev;
	else
		g_tail = n->prev;
	n->prev = n->next = NULL;
}

/* g_lock held. Push to the MRU (head) end. */
static void list_push_front(struct lru_node *n)
{
	n->prev = NULL;
	n->next = g_head;
	if (g_head)
		g_head->prev = n;
	g_head = n;
	if (!g_tail)
		g_tail = n;
}

/* g_lock held. Insert into the hash table. */
static void hash_insert(struct lru_node *n)
{
	unsigned b = hash_path(n->path);
	n->hnext = g_buckets[b];
	g_buckets[b] = n;
}

/* g_lock held. Remove from the hash table. */
static void hash_remove(struct lru_node *n)
{
	struct lru_node **pp = &g_buckets[hash_path(n->path)];
	while (*pp && *pp != n)
		pp = &(*pp)->hnext;
	if (*pp)
		*pp = n->hnext;
}

/* g_lock held. Detach + free a node and uncount its size. */
static void node_destroy(struct lru_node *n)
{
	list_unlink(n);
	hash_remove(n);
	g_total -= n->size;
	free(n->path);
	free(n);
}

void lru_touch(const char *path, off_t size)
{
	pthread_mutex_lock(&g_lock);
	struct lru_node *n = find(path);
	if (n) {
		g_total += size - n->size;
		n->size = size;
		list_unlink(n);
		list_push_front(n);
	} else {
		n = calloc(1, sizeof(*n));
		if (n) {
			n->path = strdup(path);
			if (!n->path) {
				free(n);
			} else {
				n->size = size;
				g_total += size;
				hash_insert(n);
				list_push_front(n);
			}
		}
	}
	pthread_mutex_unlock(&g_lock);
}

void lru_pin(const char *path, int delta)
{
	pthread_mutex_lock(&g_lock);
	struct lru_node *n = find(path);
	if (!n) {
		/* An open of a not-yet-tracked file: track it (size unknown yet,
		 * filled in by a later touch) so the pin has something to hold. */
		n = calloc(1, sizeof(*n));
		if (n) {
			n->path = strdup(path);
			if (!n->path) {
				free(n);
				n = NULL;
			} else {
				hash_insert(n);
				list_push_front(n);
			}
		}
	}
	if (n) {
		n->open += delta;
		if (n->open < 0)
			n->open = 0;
	}
	pthread_mutex_unlock(&g_lock);
}

int lru_is_pinned(const char *path)
{
	pthread_mutex_lock(&g_lock);
	struct lru_node *n = find(path);
	int pinned = n && n->open > 0;
	pthread_mutex_unlock(&g_lock);
	return pinned;
}

off_t lru_forget(const char *path)
{
	pthread_mutex_lock(&g_lock);
	struct lru_node *n = find(path);
	off_t size = 0;
	if (n) {
		size = n->size;
		node_destroy(n);
	}
	pthread_mutex_unlock(&g_lock);
	return size;
}

void lru_rename(const char *old_path, const char *new_path)
{
	pthread_mutex_lock(&g_lock);
	struct lru_node *n = find(old_path);
	if (n) {
		char *np = strdup(new_path);
		if (np) {
			/* Drop any existing node at the destination (overwrite). */
			struct lru_node *t = find(new_path);
			if (t && t != n)
				node_destroy(t);
			hash_remove(n);
			free(n->path);
			n->path = np;
			hash_insert(n);
		}
	}
	pthread_mutex_unlock(&g_lock);
}

off_t lru_total(void)
{
	pthread_mutex_lock(&g_lock);
	off_t t = g_total;
	pthread_mutex_unlock(&g_lock);
	return t;
}

size_t lru_snapshot_lru(char out[][PATH_MAX], size_t cap)
{
	size_t i = 0;
	pthread_mutex_lock(&g_lock);
	for (struct lru_node *n = g_tail; n && i < cap; n = n->prev) {
		if (n->open > 0)
			continue; /* pinned: not an eviction candidate */
		snprintf(out[i++], PATH_MAX, "%s", n->path);
	}
	pthread_mutex_unlock(&g_lock);
	return i;
}
