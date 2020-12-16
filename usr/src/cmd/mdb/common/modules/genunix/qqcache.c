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
 * Copyright 2018, Joyent, Inc.
 */

#include <mdb/mdb_modapi.h>
#include <mdb/mdb_ctf.h>

#include <sys/qqcache.h>
#include <sys/qqcache_impl.h>

#include "qqcache.h"

typedef struct qqcache_walk_data {
	size_t	qwd_link_off;
} qqcache_walk_data_t;

typedef struct mdb_qqcache {
	size_t qqc_link_off;
	size_t qqc_nbuckets;
} mdb_qqcache_t;

static int
qqcache_walk_init(mdb_walk_state_t *wsp, boolean_t use_hash)
{
	qqcache_walk_data_t *qwd;
	uintptr_t base;
	size_t i, n, qqc_list_sz;
	int cache_off, bucket_off, list_off;
	mdb_qqcache_t qc;

	/*  mdb_ctf_offsetof_by_name will print any errors */
	cache_off = mdb_ctf_offsetof_by_name("qqcache_t", "qqc_lists");
	if (cache_off == -1)
		return (WALK_ERR);

	bucket_off = mdb_ctf_offsetof_by_name("qqcache_t", "qqc_buckets");
	if (bucket_off == -1)
		return (WALK_ERR);

	list_off = mdb_ctf_offsetof_by_name("qqcache_list_t", "qqcl_list");
	if (list_off == -1)
		return (WALK_ERR);

	/* mdb_ctf_sizeof_by_name will print any errors */
	qqc_list_sz = mdb_ctf_sizeof_by_name("qqcache_list_t");
	if (qqc_list_sz == -1)
		return (WALK_ERR);

	if (mdb_ctf_vread(&qc, "qqcache_t", "mdb_qqcache_t", wsp->walk_addr,
	    0) == -1) {
		mdb_warn("failed to read qqcache_t at %#lx", wsp->walk_addr);
		return (WALK_ERR);
	}

	qwd = wsp->walk_data = mdb_zalloc(sizeof (*qwd), UM_SLEEP);
	qwd->qwd_link_off = qc.qqc_link_off;

	if (use_hash) {
		base = wsp->walk_addr + bucket_off;
		n = qc.qqc_nbuckets;
	} else {
		base = wsp->walk_addr + cache_off;
		n = QQCACHE_NUM_LISTS;
	}

	for (i = 0; i < n; i++) {
		wsp->walk_addr = base + i * qqc_list_sz + list_off;

		if (mdb_layered_walk("list", wsp) == -1) {
			mdb_warn("can't walk qqcache_t");
			mdb_free(qwd, sizeof (*qwd));
			return (WALK_ERR);
		}
	}

	return (WALK_NEXT);
}

int
qqcache_walk_init_cache(mdb_walk_state_t *wsp)
{
	return (qqcache_walk_init(wsp, B_FALSE));
}

int
qqcache_walk_init_hash(mdb_walk_state_t *wsp)
{
	return (qqcache_walk_init(wsp, B_TRUE));
}

int
qqcache_walk_step(mdb_walk_state_t *wsp)
{
	qqcache_walk_data_t *qwd = wsp->walk_data;
	uintptr_t addr = wsp->walk_addr - qwd->qwd_link_off;

	return (wsp->walk_callback(addr, wsp->walk_layer, wsp->walk_cbdata));
}

void
qqcache_walk_fini(mdb_walk_state_t *wsp)
{
	qqcache_walk_data_t *qwd = wsp->walk_data;

	mdb_free(qwd, sizeof (*qwd));
}
