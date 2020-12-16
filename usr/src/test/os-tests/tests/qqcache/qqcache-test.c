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
#include <err.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <sys/debug.h>
#include <sys/list.h>
#include <sys/types.h>
#include <sys/qqcache.h>
#include <sys/qqcache_impl.h>
#include <umem.h>

/* Some arbitrary sizes  */
#define	INITIAL_CACHE_SIZE	12
#define	INITIAL_CACHE_A		25
#define	CACHE_HSIZE		11

#define	OUTPUT_WIDTH	80

/*
 * If we extend the implementation to use more lists, the test code will need
 * to be updated accordingly
 */
CTASSERT(QQCACHE_NUM_LISTS == 2);

typedef struct entry {
	uint_t		e_val;
	qqcache_link_t	e_link;
} entry_t;

enum {
	ITER_ERROR = -1,
	ITER_OK = 0,
	ITER_STOP = 1
};

static uint64_t entry_hash(const void *);
static int entry_cmp(const void *, const void *);
static void entry_dtor(void *);
static entry_t *entry_new(uint_t val);

static void expect(qqcache_t *, uint_t *, size_t, uint_t *, size_t, int);
static void expect_val(qqcache_t *, const entry_t *, uint_t);
static void dump_cache(qqcache_t *);
static int iter_list(qqcache_t *, size_t, int (*)(void *, void *), void *);
static int xprintf(FILE *, const char *, ...);

int
main(void)
{
	qqcache_t *qc;
	uint_t val;

	VERIFY0(qqcache_create(&qc, INITIAL_CACHE_SIZE, INITIAL_CACHE_A,
	    CACHE_HSIZE, entry_hash, entry_cmp, entry_dtor, sizeof (entry_t),
	    offsetof(entry_t, e_link), offsetof(entry_t, e_val), UMEM_DEFAULT));

	/* Create a few entries */
	VERIFY0(qqcache_insert(qc, entry_new(5)));
	VERIFY0(qqcache_insert(qc, entry_new(4)));
	VERIFY0(qqcache_insert(qc, entry_new(3)));
	VERIFY0(qqcache_insert(qc, entry_new(2)));
	VERIFY0(qqcache_insert(qc, entry_new(1)));
	/*CSTYLED*/
	expect(qc, NULL, 0, (uint_t[]){1, 2, 3, 4, 5}, 5, __LINE__);

	/* Adding a duplicate should fail */
	{
		entry_t *e = entry_new(3);
		VERIFY3S(qqcache_insert(qc, e), ==, EEXIST);
		entry_dtor(e);
	}

	/*
	 * cstyle currently cannot handle compound literals, so just
	 * quiet it for now.
	 */

	/* BEGIN CSTYLED */
	VERIFY0(qqcache_insert(qc, entry_new(10)));
	VERIFY0(qqcache_insert(qc, entry_new(9)));
	VERIFY0(qqcache_insert(qc, entry_new(8)));
	VERIFY0(qqcache_insert(qc, entry_new(7)));
	/*  This should bump the LRU entry (5) from the list */
	VERIFY0(qqcache_insert(qc, entry_new(6)));
	expect(qc, NULL, 0,
	    (uint_t[]){6, 7, 8, 9, 10, 1, 2, 3, 4}, 9, __LINE__);

	/* Lookup a few entries to move them to the MFU list */
	val = 3;
	expect_val(qc, qqcache_lookup(qc, &val), 3);
	expect(qc, (uint_t[]) {3}, 1,
	    (uint_t[]){6, 7, 8, 9, 10, 1, 2, 4}, 8, __LINE__);

	val = 8;
	expect_val(qc, qqcache_lookup(qc, &val), 8);
	expect(qc, (uint_t[]) {8, 3}, 2,
	    (uint_t[]){6, 7, 9, 10, 1, 2, 4}, 7, __LINE__);

	/* Now move 3 back to the head of list 0 */
	val = 3;
	expect_val(qc, qqcache_lookup(qc, &val), 3);
	expect(qc, (uint_t[]) {3, 8}, 2,
	    (uint_t[]){6, 7, 9, 10, 1, 2, 4}, 7, __LINE__);

	val = 7;
	expect_val(qc, qqcache_lookup(qc, &val), 7);
	expect(qc, (uint_t[]) {7, 3, 8}, 3,
	    (uint_t[]){6, 9, 10, 1, 2, 4}, 6, __LINE__);

	/* This should push 8 from the MFU back onto the MRU */
	val = 10;
	expect_val(qc, qqcache_lookup(qc, &val), 10);
	expect(qc, (uint_t[]) {10, 7, 3}, 3,
	    (uint_t[]){8, 6, 9, 1, 2, 4}, 6, __LINE__);

	/* Add some more values */
	VERIFY0(qqcache_insert(qc, entry_new(11)));
	VERIFY0(qqcache_insert(qc, entry_new(12)));
	VERIFY0(qqcache_insert(qc, entry_new(13)));
	VERIFY0(qqcache_insert(qc, entry_new(14)));
	VERIFY0(qqcache_insert(qc, entry_new(15)));
	expect(qc, (uint_t[]) {10, 7, 3}, 3,
	    (uint_t[]){15, 14, 13, 12, 11, 8, 6, 9, 1, 2}, 9, __LINE__);

	VERIFY0(qqcache_adjust_size(qc, INITIAL_CACHE_SIZE + 4));
	expect(qc, (uint_t[]) {10, 7, 3}, 3,
	    (uint_t[]){15, 14, 13, 12, 11, 8, 6, 9, 1, 2}, 9, __LINE__);

	VERIFY0(qqcache_insert(qc, entry_new(16)));
	VERIFY0(qqcache_insert(qc, entry_new(17)));
	VERIFY0(qqcache_insert(qc, entry_new(18)));
	VERIFY0(qqcache_insert(qc, entry_new(19)));
	expect(qc, (uint_t[]) {10, 7, 3}, 3,
	    (uint_t[]) {19, 18, 17, 16, 15, 14, 13, 12, 11, 8, 6, 9}, 12,
	    __LINE__);

	VERIFY0(qqcache_adjust_size(qc, INITIAL_CACHE_SIZE - 2));
	expect(qc, (uint_t[]) {10, 7}, 2,
	    (uint_t[]){3, 19, 18, 17, 16, 15, 14, 13}, 8, __LINE__);

	VERIFY3S(qqcache_adjust_size(qc, 2), ==, EINVAL);

	VERIFY0(qqcache_adjust_a(qc, 50));
	expect(qc, (uint_t[]) {10, 7}, 2,
	    (uint_t[]){3, 19, 18, 17, 16}, 5, __LINE__);

	/* END CSTYLED */

	qqcache_destroy(qc);
	return (0);
}

struct cmp_arg {
	qqcache_t *qc;
	uint_t *vals;
	size_t i;
	size_t listnum;
	int linenum;
};

static int
cmp_cb(void *op, void *arg)
{
	entry_t *e = op;
	struct cmp_arg *ca = arg;
	uint_t val = ca->vals[ca->i++];

	if (e->e_val == val)
		return (ITER_OK);

	(void) xprintf(stderr, "Line %d: Unexpected value in list %zu.\n",
	    ca->linenum, ca->listnum);
	(void) xprintf(stderr, "    Expected: %u\n      Actual: %u\n", val,
	    e->e_val);

	return (ITER_ERROR);
}

static void
cmp_list(qqcache_t *qc, size_t listnum, uint_t *vals, size_t n, int linenum)
{
	qqcache_list_t *l = &qc->qqc_lists[listnum];
	struct cmp_arg arg = {
		.qc = qc,
		.vals = vals,
		.i = 0,
		.listnum = listnum,
		.linenum = linenum
	};

	if (l->qqcl_len != n) {
		(void) xprintf(stderr,
		    "Line %d: Unexpected length for list %zu.\n"
		    "    Length: %zu\n"
		    "  Expected: %zu\n\n", linenum, listnum, l->qqcl_len, n);
		dump_cache(qc);
	}

	if (iter_list(qc, listnum, cmp_cb, &arg) != ITER_OK) {
		dump_cache(qc);
		exit(1);
	}
}

static void
expect(qqcache_t *qc, uint_t *l0, size_t l0sz, uint_t *l1, size_t l1sz,
    int linenum)
{
	cmp_list(qc, 0, l0, l0sz, linenum);
	cmp_list(qc, 1, l1, l1sz, linenum);
}

static void
expect_val(qqcache_t *qc, const entry_t *e, uint_t val)
{
	char buf[2][64];
	if (e == NULL && val == UINT_MAX)
		return;

	if (e != NULL && e->e_val == val)
		return;

	if (e != NULL)
		(void) snprintf(buf[0], sizeof (buf[0]), "%u", e->e_val);
	else
		(void) strlcpy(buf[0], "<NULL>", sizeof (buf[0]));

	if (val != UINT_MAX)
		(void) snprintf(buf[1], sizeof (buf[1]), "%u", val);
	else
		(void) strlcpy(buf[1], "<NONE>", sizeof (buf[1]));

	(void) xprintf(stderr, "Unexpected value in list:\n");
	(void) xprintf(stderr, "    Found: %s\n Expected: %s\n",
	    buf[0], buf[1]);
	dump_cache(qc);
	exit(1);
}

struct dump_args {
	int	prefixlen;
	int	col;
	boolean_t nl;
};

static int
dump_entry(void *ep, void *arg)
{
	entry_t *e = ep;
	struct dump_args *da = arg;
	char buf[64] = { 0 };
	int n;

	n = snprintf(buf, sizeof (buf), "%u", e->e_val);
	/* buf should be large enough to hold an unsigned val */
	VERIFY3S(n, >, 0);
	VERIFY3S(n, <, sizeof (buf));

	if (da->col + n + 2 > OUTPUT_WIDTH) {
		da->col = xprintf(stderr, "\n%*s", da->prefixlen, "") - 1;
		da->nl = B_TRUE;
	} else if (!da->nl) {
		da->col += xprintf(stderr, ", ");
	}

	da->col += xprintf(stderr, "%s", buf);
	da->nl = B_FALSE;

	return (ITER_OK);
}

static void
dump_cache(qqcache_t *qc)
{
	(void) xprintf(stderr, "Cache contents:\n");

	for (size_t i = 0; i < QQCACHE_NUM_LISTS; i++) {
		qqcache_list_t *l = &qc->qqc_lists[i];
		struct dump_args args = {
			.nl = B_TRUE
		};

		args.col = args.prefixlen =
		    xprintf(stderr, "List %zu (%zu/%zu): ", i, l->qqcl_len,
		    qc->qqc_max[i]);

		(void) iter_list(qc, i, dump_entry, &args);
		VERIFY(fputc('\n', stderr));
	}
}

static int
iter_list(qqcache_t *qc, size_t listnum, int (*cb)(void *, void *),
    void *arg)
{
	qqcache_list_t *l = &qc->qqc_lists[listnum];
	void *lp;
	int ret;

	for (lp = list_head(&l->qqcl_list); lp != NULL;
	    lp = list_next(&l->qqcl_list, lp)) {
		if ((ret = cb(link_to_obj(qc, lp), arg)) != ITER_OK)
			return (ret);
	}

	return (ITER_OK);
}

/*
 * A small wrapper around vfprintf(3C) so caller doesn't need to deal with
 * errors or negative return values.
 */
static int
xprintf(FILE *f, const char *fmt, ...)
{
	int n;
	va_list ap;

	va_start(ap, fmt);
	n = vfprintf(f, fmt, ap);
	va_end(ap);

	if (n < 0 || ferror(f))
		err(EXIT_FAILURE, "\nUnable to write output");

	return (n);
}

static entry_t *
entry_new(uint_t val)
{
	entry_t *e = calloc(1, sizeof (*e));

	VERIFY3P(e, !=, NULL);
	e->e_val = val;
	return (e);
}

static uint64_t
entry_hash(const void *p)
{
	const uint_t *vp = p;
	uint64_t val = *vp;
	return (val);
}

static int
entry_cmp(const void *a, const void *b)
{
	const uint_t *l = a;
	const uint_t *r = b;
	return ((*l == *r) ? 0 : 1);
}

static void
entry_dtor(void *arg)
{
	free(arg);
}

const char *
_umem_debug_init(void)
{
	return ("default,verbose");
}

const char *
_umem_logging_init(void)
{
	return ("fail,contents");
}
