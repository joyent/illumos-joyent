/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2020 Joyent, Inc.
 */

#ifndef _QQCACHE_H
#define	_QQCACHE_H

#include <sys/list.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * This implements a fixed-size hash table that uses the 2Q algorithm
 * from Johnson and Shasha to manage the contents of the entries.
 *
 * Briefly, there are two fixed sizes lists (0 and 1).  New entries are
 * added to the head of list 1, and upon subsequent access (lookup), are
 * moved to the head of list 0.  Entries that fall off the end of list 0
 * are pushed onto the head of list 1, and entries that fall off the end
 * of list 1 are deleted.  The percentage of the total size of the cache
 * for each list is determined by the parameter 'a', which is a percentage
 * (0-100) of the cache size that is dedicated to list 0.
 *
 * This implementation does generalize this algorithm somewhat to an
 * arbitrary number of lists (instead of just 2) via the QQCACHE_NUM_LISTS
 * and QQCACHE_INSERT_LIST preprocessor symbols (defined in
 * sys/qqcache_impl.h).  New entries are added to list QQCACHE_INSERT_LIST
 * and as each list gets full, the oldest entry in each list is pushed to
 * the head of the succeeding list, and the oldest entries are removed
 * from the cache (so each list never has more entries than their maximum
 * size).
 *
 * The API itself is very similar to that of refhash.  A qqcache_link_t struct
 * is embedded within the definition of the entries that are being stored in
 * a given qqcache_t.  Functions are provided to hash/compare the tag (key)
 * value of an entry, as well as destroying the entry during the creation
 * of the cache.  Lookups then occur by passing a pointer to the key value
 * being looked up.
 *
 * NOTE: As one can take references to entries in the cache via the
 * qqcache_hold() function, refheld entries that are marked for deletion are
 * not counted when tracking the cache size, and their dtor function is not
 * called until the last reference has been released (by calling the
 * qqcache_rele() function).
 */

typedef enum qqcache_flag {
	QQCACHE_F_DEAD	= 0x01,
} qqcache_flag_t;

typedef struct qqcache_link {
	list_node_t	qqln_hash_link;	/* Hash chain bucket */
	list_node_t	qqln_list_link; /* Cache list link */
	uint_t		qqln_listnum;
	uint_t		qqln_refcnt;
	qqcache_flag_t	qqln_flags;
} qqcache_link_t;

struct qqcache;
typedef struct qqcache qqcache_t;

typedef uint64_t (*qqcache_hash_fn_t)(const void *);
typedef int (*qqcache_cmp_fn_t)(const void *, const void *);
typedef void (*qqcache_dtor_fn_t)(void *);

/*
 * qqcache_create(qcp, sz, a, buckets, hash_fn, cmp_fn, dtor_fn,
 *    elsize, link_off, tag_off, flags);
 *
 * Creates a new 2Q cache:
 *
 *	qqcache_t **qcp	A pointer to the pointer that will hold the new
 *			cache.
 *
 *	size_t sz	The size of the cache (in entries).
 *
 *	size_t a	The percentage (0-100) of the cache dedicated to
 *			MRU entries (list 0);
 *
 *	size_t buckets	The number of hash buckets in the cache.
 *
 *	qqcache_hash_fn_t hash_fn	The function used to create a
 *					hash value for a given entry's tag
 *					value.
 *
 *	qqcache_cmp_fn_t cmp_fn		The function used to compare the two
 *					tag values of two entries.  The function
 *					should return '0' if the two entries
 *					are equal, '1' if they are not equal.
 *
 *	qqcache_dtor_fn_t dtor_fn	The function used to destroy/free
 *					entries.
 *
 *	size_t elsize	The size of each entry.
 *
 *	size_t link_off	The offset of the qqcache_link_t struct in the entry.
 *
 *	size_t tag_off	The offset in the entry of the tag value (used for
 *			hashing and comparison).
 *
 *	int flags	The flags passed to kmem_zalloc/umem_zalloc.
 *
 * Returns:
 *	0	Success
 *	EINVAL	A parameter was not valid
 *	ENOMEM	The memory allocation failed (only possible when
 *		KM_NOSLEEP/UMEM_DEFAULT is passed to flags).
 */
extern int qqcache_create(qqcache_t **, size_t, size_t, size_t,
    qqcache_hash_fn_t, qqcache_cmp_fn_t, qqcache_dtor_fn_t,
    size_t, size_t, size_t, int);

/* Destroy the given qqcache_t */
extern void qqcache_destroy(qqcache_t *);

/*
 * qqcache_insert(qc, obj)
 *
 * qqcache_t *qc	The cache to insert the item into.
 *
 * void *obj		The object to add.
 *
 * Returns:
 *	0	Success
 *	EEXIST	The same entry (as determined by the cache cmp function) already
 *		exists in the cache.
 */
extern int qqcache_insert(qqcache_t *, void *);

/* Lookup an entry with the given tag/key, or return NULL if not found */
extern void *qqcache_lookup(qqcache_t *, const void *);

/* Remove the given entry from the cache */
extern void qqcache_remove(qqcache_t *, void *);

/* Add a hold on the entry in the cache */
extern void qqcache_hold(qqcache_t *, void *);

/* Release the hold on the entry in the cache */
extern void qqcache_rele(qqcache_t *, void *);

/*
 * Adjust the size and percentage of the cache for list 0.  If new values are
 * smaller than current values, entries may be evicted as necessary to reduce
 * the size of the cache to the given size.
 */
extern int qqcache_adjust_size(qqcache_t *, size_t);
extern int qqcache_adjust_a(qqcache_t *, size_t);

/* Return the current values of size or a. */
extern size_t qqcache_size(const qqcache_t *);
extern size_t qqcache_a(const qqcache_t *);

extern size_t qqcache_nentries(const qqcache_t *);

/* Iterate through entries. */
extern void *qqcache_first(qqcache_t *);
extern void *qqcache_next(qqcache_t *, void *);

/* Lookup stats */
extern uint64_t qqcache_lookups(const qqcache_t *);
extern uint64_t qqcache_hits(const qqcache_t *);

#ifdef __cplusplus
}
#endif

#endif /* _QQCACHE_H */
