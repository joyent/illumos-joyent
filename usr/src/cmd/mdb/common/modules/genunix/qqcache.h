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
 * Copyright 2018, Joyent Inc.
 */

#ifndef _MDB_QQCACHE_H
#define	_MDB_QQCACHE_H

#ifdef __cplusplus
extern "C" {
#endif

#define	QQCACHE_WALK_NAME "qqcache"
#define	QQCACHE_WALK_DESC "walk a qqcache (2Q cache)"

#define	QQCACHE_HASH_WALK_NAME "qqhash"
#define	QQCACHE_HASH_WALK_DESC "walk a qqcache (2Q cache) via the hash buckets"

struct mdb_walk_state;

extern int qqcache_walk_init_cache(struct mdb_walk_state *);
extern int qqcache_walk_init_hash(struct mdb_walk_state *);
extern int qqcache_walk_step(struct mdb_walk_state *);
extern void qqcache_walk_fini(struct mdb_walk_state *);

#ifdef __cplusplus
}
#endif

#endif /* _MDB_QQCACHE_H */
