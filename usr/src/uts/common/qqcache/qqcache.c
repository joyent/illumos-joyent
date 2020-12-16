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

#include <sys/debug.h>
#include <sys/errno.h>
#include <sys/null.h>
#include <sys/types.h>
#include <sys/qqcache.h>
#include <sys/qqcache_impl.h>
#include <sys/stddef.h>

/*
 * Currently, the non _KERNEL pieces are to support testing in usr/src/test.
 */
#ifdef _KERNEL
#include <sys/kmem.h>
#define	ZALLOC kmem_zalloc
#define	FREE kmem_free
#else
#include <umem.h>
#define	ZALLOC umem_zalloc
#define	FREE umem_free
#endif


/*
 * The *_overflow functions mimic the gcc/clang intrinsic functions.  Once
 * we are using a newer compiler version to that includes these as intrisnics,
 * these can be replaced with those versions.
 */
static int
uadd_overflow(const size_t a, const size_t b, size_t *sump)
{
	*sump = a + b;
	if (*sump < a || *sump < b)
		return (1);
	return (0);
}

#define	MUL_NO_OVERFLOW ((size_t)1 << (sizeof (size_t) * 4))

static int
umul_overflow(const size_t a, const size_t b, size_t *cp)
{
	*cp = a * b;

	if ((a >= MUL_NO_OVERFLOW || b >= MUL_NO_OVERFLOW) &&
	    a != 0 && b != 0 && SIZE_MAX / a < b)
		return (1);

	return (0);
}

/* Calculate the capacity of each list based on sz and a */
static void
qqcache_size_lists(size_t sz, size_t a, size_t *maxp)
{
	VERIFY3U(sz, >=, QQCACHE_NUM_LISTS);

	/*
	 * The general approach is to start with list 0 being sized as a% of
	 * sz.  However every other list must be able to hold at least one
	 * entry unless a == 100 (i.e. 100%).  If the straight percentage
	 * leaves any of the remaining lists with zero entries, we give them
	 * a size of 1, and then adjust list0's size according so that the
	 * sum off all list sizes == sz (this is mostly only a concern where
	 * sz is small enough such that (100 - a)% of sz < QQCACHE_NUM_LISTS).
	 */
	size_t list0sz = sz * a / 100;
	size_t othersz = (sz - list0sz) / (QQCACHE_NUM_LISTS - 1);

	if (list0sz == 0)
		list0sz = 1;

	if (othersz == 0 && a != 100)
		othersz = 1;

	if (list0sz + othersz * (QQCACHE_NUM_LISTS - 1) > sz)
		list0sz = sz - othersz * (QQCACHE_NUM_LISTS - 1);

	maxp[0] = list0sz;
	for (size_t i = 1; i < QQCACHE_NUM_LISTS; i++)
		maxp[i] = othersz;
}

int
qqcache_create(qqcache_t **qp, size_t sz, size_t a, size_t buckets,
    qqcache_hash_fn_t hash_fn, qqcache_cmp_fn_t cmp_fn,
    qqcache_dtor_fn_t dtor_fn, size_t elsize, size_t link_off, size_t tag_off,
    int kmflags)
{
	qqcache_t *qc;
	size_t len = 0;

	if (sz < QQCACHE_MIN_SIZE)
		return (EINVAL);
	if (a > 100)
		return (EINVAL);

	if (umul_overflow(sizeof (qqcache_list_t), buckets, &len))
		return (EINVAL);
	if (uadd_overflow(sizeof (*qc), len, &len))
		return (EINVAL);

	if ((qc = ZALLOC(len, kmflags)) == NULL)
		return (ENOMEM);

	qc->qqc_hash_fn = hash_fn;
	qc->qqc_cmp_fn = cmp_fn;
	qc->qqc_dtor_fn = dtor_fn;
	qc->qqc_link_off = link_off;
	qc->qqc_tag_off = tag_off;
	qc->qqc_nbuckets = buckets;
	qc->qqc_size = sz;
	qc->qqc_a = a;

	qqcache_size_lists(sz, a, qc->qqc_max);

	for (size_t i = 0; i < buckets; i++) {
		list_create(&qc->qqc_buckets[i].qqcl_list, elsize,
		    offsetof(qqcache_link_t, qqln_hash_link));
	}

	for (size_t i = 0; i < QQCACHE_NUM_LISTS; i++) {
		list_create(&qc->qqc_lists[i].qqcl_list, elsize,
		    offsetof(qqcache_link_t, qqln_list_link));
	}

	*qp = qc;
	return (0);
}

void
qqcache_destroy(qqcache_t *qc)
{
	size_t len;

	if (qc == NULL)
		return;

	/* If creation succeeded, this calculation cannot overflow */
	len = sizeof (*qc) + qc->qqc_nbuckets * sizeof (qqcache_list_t);

	for (size_t i = 0; i < QQCACHE_NUM_LISTS; i++) {
		list_t *l = &qc->qqc_lists[i].qqcl_list;
		qqcache_link_t *lnk;

		while ((lnk = list_remove_head(l)) != NULL)
			;
	}

	for (size_t i = 0; i < qc->qqc_nbuckets; i++) {
		list_t *l = &qc->qqc_buckets[i].qqcl_list;
		qqcache_link_t *lnk;

		while ((lnk = list_remove_head(l)) != NULL) {
			ASSERT0(lnk->qqln_refcnt);
			qc->qqc_dtor_fn(link_to_obj(qc, lnk));
		}
	}

	FREE(qc, len);
}

/*
 * Removal of an entry is a two step process.  qqcache_remove() removes the
 * entry from the cache lists, and if a reference is held, sets the
 * QQCACHE_F_DEAD flag.  When there are no more references held on an entry,
 * (either none are held at the time qqcache_remove() is called, or the last
 * reference is removed via qqcache_rele(), qqcache_delete() is called which
 * removes the entry from its hash bucket and calls the entry's dtor function.
 *
 * The main reason for the two step process is largely simplicity.  If the
 * entry remains in the cache lists w/ the QQCACHE_F_DEAD flag set, it
 * complicates keeping each cache within its size limits -- either the
 * list size must reflect the number of non-dead entries (which could be
 * confusing during troubleshooting), or as we push things down the list, we
 * would need to skip/ignore dead entries.  The hash buckets however don't
 * have any size limits (to impose limits would require the hash function
 * provided by the consumer to produce perfectly equal distribution of entries
 * across all the hash buckets at all times).  The only time we care about
 * the QQCACHE_F_DEAD flag in the hash buckets is when trying to lookup a
 * 'dead' value, so leaving the entries in there does not present the same
 * issues as leaving them in the hash buckets (while still providing a way to
 * find refheld entries).
 */
static void
qqcache_delete(qqcache_t *qc, qqcache_link_t *lp)
{
	void *op = link_to_obj(qc, lp);
	void *tp = obj_to_tag(qc, op);
	uint_t n = qc->qqc_hash_fn(tp) % qc->qqc_nbuckets;

	ASSERT3U(qc->qqc_buckets[n].qqcl_len, >, 0);
	ASSERT(!list_is_empty(&qc->qqc_buckets[n].qqcl_list));
	ASSERT(!list_link_active(&lp->qqln_list_link));
	ASSERT(list_link_active(&lp->qqln_hash_link));

	list_remove(&qc->qqc_buckets[n].qqcl_list, lp);
	qc->qqc_buckets[n].qqcl_len--;
	qc->qqc_dtor_fn(op);
}

void
qqcache_remove(qqcache_t *qc, void *op)
{
	qqcache_link_t *lp = obj_to_link(qc, op);
	qqcache_list_t *lst = QQCACHE_LIST(qc, lp);

	ASSERT(!list_is_empty(&lst->qqcl_list));
	ASSERT3U(lst->qqcl_len, >, 0);

	list_remove(&lst->qqcl_list, lp);
	lst->qqcl_len--;

	if (lp->qqln_refcnt > 0)
		lp->qqln_flags |= QQCACHE_F_DEAD;
	else
		qqcache_delete(qc, lp);
}

void
qqcache_hold(qqcache_t *qc, void *op)
{
	qqcache_link_t *lp = obj_to_link(qc, op);

	++lp->qqln_refcnt;
}

void
qqcache_rele(qqcache_t *qc, void *op)
{
	qqcache_link_t *lp = obj_to_link(qc, op);

	VERIFY3U(lp->qqln_refcnt, >, 0);

	if (--lp->qqln_refcnt == 0 && (lp->qqln_flags & QQCACHE_F_DEAD))
		qqcache_delete(qc, lp);
}

static qqcache_link_t *
qqcache_hash_lookup(qqcache_t *qc, const void *tp, qqcache_list_t **lpp)
{
	uint_t n = qc->qqc_hash_fn(tp) % qc->qqc_nbuckets;
	qqcache_link_t *lp;
	qqcache_list_t *bucket = &qc->qqc_buckets[n];
	list_t *l = &bucket->qqcl_list;
	void *cmp;

	if (lpp != NULL)
		*lpp = bucket;

	for (lp = list_head(l); lp != NULL; lp = list_next(l, lp)) {
		cmp = obj_to_tag(qc, link_to_obj(qc, lp));

		if (qc->qqc_cmp_fn(cmp, tp) == 0 &&
		    !(lp->qqln_flags & QQCACHE_F_DEAD)) {
			return (lp);
		}
	}

	return (NULL);
}

/*
 * Starting at listnum, push entries from the tail of cache list 'n' to the
 * head of * list 'n + 1', keeping each list within their size limits.  Excess
 * entries on the tail of the last list are deleted.  If 'for_insert' is
 * B_TRUE, also guarantee after this returns that there are no more than
 * 'max - 1' entries on listnum (so there is room to insert an entry onto
 * listnum).
 */
static void
qqcache_ripple(qqcache_t *qc, uint_t listnum, boolean_t for_insert)
{
	VERIFY3U(listnum, <, QQCACHE_NUM_LISTS);

	for (uint_t i = listnum; i < QQCACHE_NUM_LISTS; i++) {
		qqcache_list_t *ql = &qc->qqc_lists[i];
		qqcache_list_t *qlnext = &qc->qqc_lists[i + 1];
		size_t max = qc->qqc_max[i];

		ASSERT3U(max, >, 0);

		/*
		 * If we're planning to insert an entry on list 'listnum',
		 * we bump the maximum size down by one to guarantee we
		 * have sufficient room for the entry
		 */
		if (for_insert && i == listnum)
			max--;

		while (ql->qqcl_len > max) {
			qqcache_link_t *lnk = list_tail(&ql->qqcl_list);

			if (i + 1 < QQCACHE_NUM_LISTS) {
				list_remove(&ql->qqcl_list, lnk);
				ql->qqcl_len--;

				ASSERT3U(lnk->qqln_listnum, ==, i);
				lnk->qqln_listnum++;

				list_insert_head(&qlnext->qqcl_list, lnk);
				qlnext->qqcl_len++;
			} else {
				qqcache_remove(qc, link_to_obj(qc, lnk));
			}
		}
	}
}

int
qqcache_insert(qqcache_t *qc, void *obj)
{
	qqcache_link_t *lp = obj_to_link(qc, obj);
	qqcache_list_t *bucket;

	if (qqcache_hash_lookup(qc, obj_to_tag(qc, obj), &bucket) != NULL)
		return (EEXIST);

	list_link_init(&lp->qqln_hash_link);
	list_link_init(&lp->qqln_list_link);
	lp->qqln_refcnt = 0;
	lp->qqln_flags = 0;
	lp->qqln_listnum = QQCACHE_INSERT_LIST;

	qqcache_ripple(qc, QQCACHE_INSERT_LIST, B_TRUE);

	list_insert_tail(&bucket->qqcl_list, lp);
	bucket->qqcl_len++;

	list_insert_head(&qc->qqc_lists[QQCACHE_INSERT_LIST].qqcl_list, lp);
	qc->qqc_lists[QQCACHE_INSERT_LIST].qqcl_len++;

	return (0);
}

void *
qqcache_lookup(qqcache_t *qc, const void *tp)
{
	qqcache_link_t *lp;
	qqcache_list_t *src;
	uint_t tgtnum;

	qc->qqc_lookups++;

	if ((lp = qqcache_hash_lookup(qc, tp, NULL)) == NULL)
		return (NULL);

	qc->qqc_hits++;

	src = QQCACHE_LIST(qc, lp);
	list_remove(&src->qqcl_list, lp);
	src->qqcl_len--;

	tgtnum = (lp->qqln_listnum > 0) ? lp->qqln_listnum - 1 : 0;

	if (tgtnum != lp->qqln_listnum)
		qqcache_ripple(qc, tgtnum, B_TRUE);

	lp->qqln_listnum = tgtnum;
	list_insert_head(&qc->qqc_lists[tgtnum].qqcl_list, lp);
	qc->qqc_lists[tgtnum].qqcl_len++;

	return (link_to_obj(qc, lp));
}

int
qqcache_adjust_size(qqcache_t *qc, size_t sz)
{
	if (sz < QQCACHE_MIN_SIZE)
		return (EINVAL);

	qc->qqc_size = sz;
	qqcache_size_lists(sz, qc->qqc_a, qc->qqc_max);
	qqcache_ripple(qc, 0, B_FALSE);
	return (0);
}

int
qqcache_adjust_a(qqcache_t *qc, size_t a)
{
	if (a > 100)
		return (EINVAL);

	qc->qqc_a = a;
	qqcache_size_lists(qc->qqc_size, a, qc->qqc_max);
	qqcache_ripple(qc, 0, B_FALSE);
	return (0);
}

size_t
qqcache_size(const qqcache_t *qc)
{
	return (qc->qqc_size);
}

size_t
qqcache_a(const qqcache_t *qc)
{
	return (qc->qqc_a);
}

void *
qqcache_first(qqcache_t *qc)
{
	for (size_t i = 0; i < QQCACHE_NUM_LISTS; i++) {
		qqcache_list_t *l = &qc->qqc_lists[i];

		if (l->qqcl_len > 0)
			return (link_to_obj(qc, list_head(&l->qqcl_list)));
	}

	return (NULL);
}

void *
qqcache_next(qqcache_t *qc, void *obj)
{
	qqcache_link_t *lp = obj_to_link(qc, obj);
	qqcache_link_t *next;
	qqcache_list_t *l = QQCACHE_LIST(qc, lp);

	ASSERT3U(lp->qqln_listnum, <, QQCACHE_NUM_LISTS);

	if ((next = list_next(&l->qqcl_list, lp)) != NULL)
		return (link_to_obj(qc, next));

	for (size_t i = lp->qqln_listnum + 1; i < QQCACHE_NUM_LISTS; i++) {
		l = &qc->qqc_lists[i];
		if (l->qqcl_len > 0)
			return (link_to_obj(qc, list_head(&l->qqcl_list)));
	}

	return (NULL);
}

uint64_t
qqcache_lookups(const qqcache_t *qc)
{
	return (qc->qqc_lookups);
}

uint64_t
qqcache_hits(const qqcache_t *qc)
{
	return (qc->qqc_hits);
}

size_t
qqcache_nentries(const qqcache_t *qc)
{
	size_t tot = 0;

	for (size_t i = 0; i < QQCACHE_NUM_LISTS; i++)
		tot += qc->qqc_lists[i].qqcl_len;

	return (tot);
}
