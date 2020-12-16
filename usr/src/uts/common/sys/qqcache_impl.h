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

#ifndef _QQCACHE_IMPL_H
#define	_QQCACHE_IMPL_H

#include <sys/debug.h>
#include <sys/qqcache.h>

#ifdef __cplusplus
extern "C" {
#endif

#define	QQCACHE_NUM_LISTS 2
#define	QQCACHE_INSERT_LIST 1
#define	QQCACHE_MIN_SIZE 10

CTASSERT(QQCACHE_INSERT_LIST < QQCACHE_NUM_LISTS);
CTASSERT(QQCACHE_NUM_LISTS >= 2);

typedef struct qqcache_list {
	list_t		qqcl_list;
	size_t		qqcl_len;
} qqcache_list_t;

struct qqcache {
	qqcache_hash_fn_t qqc_hash_fn;
	qqcache_cmp_fn_t qqc_cmp_fn;
	qqcache_dtor_fn_t qqc_dtor_fn;
	size_t		qqc_link_off;
	size_t		qqc_tag_off;
	size_t		qqc_nbuckets;
	size_t		qqc_size;
	size_t		qqc_a;
	size_t		qqc_max[QQCACHE_NUM_LISTS];
	uint64_t	qqc_lookups;
	uint64_t	qqc_hits;
	qqcache_list_t	qqc_lists[QQCACHE_NUM_LISTS];
	qqcache_list_t	qqc_buckets[];
};

#define	QQCACHE_LIST(qqc, lnk) \
	(&(qqc)->qqc_lists[(lnk)->qqln_listnum])

#define	obj_to_link(_q, _o) \
	((qqcache_link_t *)(((char *)(_o)) + (_q)->qqc_link_off))
#define	link_to_obj(_q, _l) \
	((void *)(((char *)(_l)) - (_q)->qqc_link_off))
#define	obj_to_tag(_q, _o) \
	((void *)(((char *)(_o)) + (_q)->qqc_tag_off))

#ifdef __cplusplus
}
#endif

#endif /* _QQCACHE_IMPL_H */
