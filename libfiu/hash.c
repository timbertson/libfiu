
/*
 * Generic, simple hash table.
 *
 * Takes \0-terminated strings as keys, and void * as values.
 * It is tuned for a small number of elements (< 1000).
 */

#include <sys/types.h>		/* for size_t */
#include <stdint.h>		/* for [u]int*_t */
#include <stdbool.h>		/* for bool */
#include <stdlib.h>		/* for malloc() */
#include <string.h>		/* for memcpy()/memcmp() */
#include <stdio.h>		/* snprintf() */
#include "hash.h"

/* MurmurHash2, by Austin Appleby. The one we use.
 * It has been modify to fit into the coding style, to work on uint32_t
 * instead of ints, and the seed was fixed to a random number because it's not
 * an issue for us. The author placed it in the public domain, so it's ok to
 * use it here.
 * http://sites.google.com/site/murmurhash/ */
static uint32_t murmurhash2(const char *key, size_t len)
{
	const uint32_t m = 0x5bd1e995;
	const int r = 24;
	const uint32_t seed = 0x34a4b627;

	// Initialize the hash to a 'random' value
	uint32_t h = seed ^ len;

	// Mix 4 bytes at a time into the hash
	while (len >= 4) {
		uint32_t k = *(uint32_t *) key;

		k *= m;
		k ^= k >> r;
		k *= m;

		h *= m;
		h ^= k;

		key += 4;
		len -= 4;
	}

	// Handle the last few bytes of the input array
	switch (len) {
		case 3: h ^= key[2] << 16;
		case 2: h ^= key[1] << 8;
		case 1: h ^= key[0];
			h *= m;
	}

	// Do a few final mixes of the hash to ensure the last few
	// bytes are well-incorporated.
	h ^= h >> 13;
	h *= m;
	h ^= h >> 15;

	return h;
}

enum used_as {
	NEVER = 0,
	IN_USE = 1,
	REMOVED = 2,
};

struct entry {
	char *key;
	void *value;
	enum used_as in_use;
};

struct hash {
	struct entry *entries;
	size_t table_size;
	size_t nentries;
	void (*destructor)(void *);
};


/* Minimum table size. */
#define MIN_SIZE 10

struct hash *hash_create(void (*destructor)(void *))
{
	struct hash *h = malloc(sizeof(struct hash));
	if (h == NULL)
		return NULL;

	h->entries = malloc(sizeof(struct entry) * MIN_SIZE);
	if (h->entries == NULL) {
		free(h);
		return NULL;
	}

	memset(h->entries, 0, sizeof(struct entry) * MIN_SIZE);

	h->table_size = MIN_SIZE;
	h->nentries = 0;
	h->destructor = destructor;

	return h;
}

void hash_free(struct hash *h)
{
	int i;
	struct entry *entry;

	for (i = 0; i < h->table_size; i++) {
		entry = h->entries + i;
		if (entry->in_use == IN_USE) {
			h->destructor(entry->value);
			free(entry->key);
		}
	}

	free(h->entries);
	free(h);
}

void *hash_get(struct hash *h, const char *key)
{
	size_t pos;
	struct entry *entry;

	pos = murmurhash2(key, strlen(key)) % h->table_size;

	for (;;) {
		entry = h->entries + pos;
		if (entry->in_use == NEVER) {
			/* We got to a entry never used, no match. */
			return NULL;
		} else if (entry->in_use == IN_USE &&
				strcmp(key, entry->key) == 0) {
			/* The key matches. */
			return entry->value;
		} else {
			/* We use linear probing for now */
			pos = (pos + 1) % h->table_size;
		}
	}

	return NULL;
}

/* Internal version of hash_set.
 * It uses the key as-is (it won't copy it), and it won't resize the array
 * either. */
static bool _hash_set(struct hash *h, char *key, void *value)
{
	size_t pos;
	struct entry *entry;

	pos = murmurhash2(key, strlen(key)) % h->table_size;

	for (;;) {
		entry = h->entries + pos;
		if (entry->in_use != IN_USE) {
			entry->in_use = IN_USE;
			entry->key = key;
			entry->value = value;
			h->nentries++;
			return true;
		} else if (strcmp(key, entry->key) == 0) {
			/* The key matches, override the value. */
			h->destructor(entry->value);
			entry->value = value;
			return true;
		} else {
			/* The key doesn't match, linear probing. */
			pos = (pos + 1) % h->table_size;
		}
	}

	return false;
}

static bool resize_table(struct hash *h, size_t new_size)
{
	size_t i;
	struct entry *old_entries, *e;
	size_t old_size;

	if (new_size < MIN_SIZE) {
		/* Do not resize below minimum size */
		return true;
	}

	old_entries = h->entries;
	old_size = h->table_size;

	h->entries = malloc(sizeof(struct entry) * new_size);
	if (h->entries == NULL)
		return false;

	memset(h->entries, 0, sizeof(struct entry) * new_size);
	h->table_size = new_size;
	h->nentries = 0;

	/* Insert the old entries into the new table. We use the internal
	 * version _hash_set() to avoid copying the keys again. */
	for (i = 0; i < old_size; i++) {
		e = old_entries + i;
		if (e->in_use == IN_USE)
			_hash_set(h, e->key, e->value);
	}

	free(old_entries);

	return true;
}

bool hash_set(struct hash *h, const char *key, void *value)
{
	if ((float) h->nentries / h->table_size > 0.7) {
		/* If we're over 70% full, grow the table by 30% */
		if (!resize_table(h, h->table_size * 1.3))
			return false;
	}

	return _hash_set(h, strdup(key), value);
}


bool hash_del(struct hash *h, const char *key)
{
	size_t pos;
	struct entry *entry;

	pos = murmurhash2(key, strlen(key)) % h->table_size;

	for (;;) {
		entry = h->entries + pos;
		if (entry->in_use == NEVER) {
			/* We got to a never used key, not found. */
			return false;
		} else if (entry->in_use == IN_USE &&
				strcmp(key, entry->key) == 0) {
			/* The key matches, remove it. */
			free(entry->key);
			h->destructor(entry->value);
			entry->key = NULL;
			entry->value = NULL;
			entry->in_use = REMOVED;
			break;
		} else {
			/* The key doesn't match, linear probing. */
			pos = (pos + 1) % h->table_size;
		}
	}

	if (h->table_size > MIN_SIZE &&
			(float) h->nentries / h->table_size < 0.5) {
		/* If we're over 50% free, shrink. */
		if (!resize_table(h, h->table_size * 0.8))
			return false;
	}

	return true;

}

