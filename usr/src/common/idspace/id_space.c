/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2000, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2017, Joyent, Inc.  All rights reserved.
 */

#include <sys/ddi.h>
#include <sys/debug.h>
#include <sys/id_space.h>
#include <sys/types.h>

#ifdef _KERNEL
#include <sys/cmn_err.h>
#include <sys/bitmap.h>
#include <sys/kmem.h>
#include <sys/stddef.h>
#include <sys/sunddi.h>
#else
#include <stddef.h>
#include <stdio.h>
#include <strings.h>
#include <umem.h>
#endif
/*
 * ID Space Big Theory Statement
 *
 * 1. Overview
 *
 * The id_space_t provides an implementation of a managed range of
 * integer identifiers.  An ID space guarantees that the
 * next identifer returned by an allocation is larger than the previous one,
 * unless there are no larger slots remaining in the range.  In this case,
 * the ID space will return the first available slot in the lower part of the
 * range (viewing the previous identifier as a partitioning element).  If no
 * slots are available, id_alloc()/id_allocff() will sleep until an
 * identifier becomes available.  Accordingly, id_space allocations must be
 * initiated from contexts where sleeping is acceptable.  id_alloc_nosleep()/
 * id_allocff_nosleep() will return -1 if no slots are available or if the
 * system is low on memory.  If id_alloc_nosleep() fails, callers should
 * not try to extend the ID space.  This is to avoid making a possible
 * low-memory situation worse.
 *
 * As an ID space is designed for representing a range of id_t's, there
 * is a preexisting maximal range: [0, MAXUID].  ID space requests outside
 * that range will fail on a DEBUG kernel.  The id_allocff*() functions
 * return the first available id, and should be used when there is benefit
 * to having a compact allocated range.
 *
 * (Presently, the id_space_t abstraction supports only direct allocations; ID
 * reservation, in which an ID is allocated but placed in a internal
 * dictionary for later use, should be added when a consuming subsystem
 * arrives.)
 *
 * This code is also shared with userland. In userland, we don't have the same
 * ability to have sleeping variants, so we effectively turn the normal
 * versions without _nosleep into _nosleep.
 *
 * 2. High-Level Implementation
 *
 * The only outward-facing struct is the id_space_t - this is returned by
 * id_space_create and must be passed to the various id space allocation
 * functions. The struct contains an AVL tree of id_tree_t structs - each
 * id_tree_t represents a continuous range of IDs. A freshly created id_space_t
 * will have just one id_tree_t in its AVL tree, and an additional id_tree_t
 * will be added with every call to id_space_extend. The id_tree_t structs are
 * ordered in the AVL tree by their starting IDs, low-to-high. The ranges of ID
 * trees do not overlap.
 *
 * An id_tree_t is a tree of id_node_t structs. Each id_node_t has at most
 * ID_BRANCH_FACTOR children. Each id_node_t also has a bitfield, which will
 * serve two different purposes depending on whether or not the node is a leaf.
 * If the node is a leaf, zeroes in the bitfield will indicate unallocated IDs,
 * and ones will indicate allocated IDs. If the node is not a leaf, each bit
 * will correspond to a child node. A zero will indicate that the corresponding
 * child (or one of the child's children or its childrens' children, etc.) has
 * an unallocated ID. A one will indicate that the child has no children with
 * unallocated IDs - its branch is completely full. As more IDs are allocated,
 * the ID tree will grow in both height and number of nodes per level.
 *
 * 3. Example
 *
 * The tree starts with just the root node and grows (bottom-up!) as
 * more space is required. Here's an example. For simplicity's sake, we'll use
 * a branching factor of 2 and assume that the ID range starts at 0.
 *
 * In this hacked-together notation, `N` signifies a null pointer.
 * We start with a single node (labeled node A):
 *
 * A: bitfield:[0 0]
 *    children:[N N]
 *
 * We allocate our first id, which is 0:
 *
 * A: bitfield:[1 0]
 *    children:[N N]
 *
 * And another id, 1:
 *
 * A: bitfield:[1 1]
 *    children:[N N]
 *
 * On the next allocation, we find that the root's bitfield is completely full -
 * this is how we know we've run out of space. We add _a new root_, node B:
 *
 *           B: bitfield:[1 0]
 *              children:[A N]
 *
 *          /
 *
 * A: bitfield:[1 1]
 *    children:[N N]
 *
 * Note that _we didn't have to modify A at all_.
 *
 * On the next allocation, we see that B has a child with space remaining. This
 * child hasn't actually been allocated yet, so we allocate it:
 *
 *           B: bitfield:[1 0]
 *              children:[A C]
 *
 *         /                       \
 *
 * A: bitfield:[1 1]            C: bitfield:[1 0]
 *    children:[N N]               children:[N N]
 *
 * On the next allocation, we see that we have filled up C. This means that B is
 * now also full, so we update both nodes. Furthermore, the root is now full, so
 * we add another layer:
 *
 *                          D: bitfield:[1 0]
 *                             children:[B N]
 *
 *                       /
 *
 *              B: bitfield:[1 1]
 *                 children:[A C]
 *
 *         /                       \
 *
 * A: bitfield:[1 1]            C: bitfield:[1 1]
 *    children:[N N]               children:[N N]
 *
 * And so on and so on...
 *
 * 4. Dealing with Offsets and Ranges
 *
 * From an id_tree_t's point of view, its range of IDs always starts at 0. If
 * the user creates a range of IDs that does not start at 0, the required offset
 * is added to each ID after the ID is allocated in the tree and before the ID
 * is returned to the user. If the user places an upper bound on the ID range
 * that does not align with the number of IDs supported by a full tree,
 * each bitfield is masked off during tree traversal to artificially impose an
 * upper bound.
 *
 * When the user calls id_space_extend, existing id_tree_t structs are not
 * affected. Instead, a new id_tree_t is added to the id_space_t's AVL tree to
 * support the new range. If the user specifies a range of IDs that overlaps
 * with existing ranges, the actual range of the new id_tree_t is truncated to
 * include only the truly new IDs. This way, the ranges of the id_tree_t structs
 * will never overlap.
 *
 * 5. Time and Space Complexity
 *
 * We grow the tree from the bottom up so the IDs that correspond to specific
 * slots in each node's bitfield _never change_. For example, in the simple tree
 * above, the second slot in node C will always refer to ID 3. This allows
 * constant-time tree growth, because we do not have to iterate over all nodes
 * to update their bitfields.
 *
 * When performing allocation and deletion, we can almost always get away with
 * performing just one tree traversal of length log(ID_BRANCH_FACTOR)(n), where
 * n is ID_BRANCH_FACTOR<(current tree height). This gives a logarithmic runtime
 * for allocation and deletion.
 *
 * In some cases, we may have to perform more than one traversal - this will
 * happen if we reach a leaf that has free IDs, but all the free IDs are smaller
 * than the last allocated ID. In this case, we will have to increase our
 * desired minimum ID and traverse the tree again. However, this case is rare.
 *
 * A tree of height h will support ID_BRANCH_FACTOR^(h+1) IDs and require at
 * most branch_factor^(h) + branch_factor^(h-1) + ... + branch_factor<(0) nodes.
 * This does not seem efficient for a low branching factor (e.g. 2) but becomes
 * increasingly efficient as the branching factor increases.
 *
 * 6. Explanation of Internal Functions
 *
 * The lowest-level function related to tree traversal is get_open_slot. This
 * function takes in a bitfield and high and low indices and returns the index
 * of the first zero bit between the passed-in indices. It does not perform
 * any wrap-around logic, and does not know about the structure of the tree.
 *
 * One layer of abstraction up, we have id_tree_walk. It traverses a single
 * id_tree_t and performs the specified action (allocate or free) on either the
 * exact ID passed in or the first open ID larger than the "start" ID passed in.
 * It does not perform any wrap-around logic - for example, if the passed-in
 * tree has no open IDs larger than "start" but does have open IDs smaller than
 * start, this function will NOT wrap around and return a smaller ID. This
 * function does not know about the AVL tree or any id_tree_t structs other than
 * the one passed in. This function sees ID values from the id_tree_t's point
 * of view - it does not apply offsets to create an accurate, client-facing ID
 * value. This function calls id_tree_expand to grow the height of the tree,
 * and directly allocates internal nodes if necessary during the tree traversal.
 *
 * At the outer layer of abstraction, we have id_get_id. This function traverses
 * the AVL tree and finds a suitable id_tree_t on which to call id_tree_walk.
 * This is where all wrap-around logic occurs - this function WILL return an
 * open ID smaller than the passed-in "start" ID if necessary. This is also
 * where offsets are applied to the ID returned from id_tree_walk to return
 * a client-facing ID value.
 *
 * The various client-facing ID allocation and freeing functions are thin
 * wrappers around id_get_id. They SHOULD NOT call any other internal functions,
 * as this will short-circuit important parts of the logic.
 *
 * Sleeping when waiting for an ID to become free is accomplished using a
 * condvar in the client-facing ID allocation functions. The internal functions
 * should not touch the condvar.
 */

#ifndef _KERNEL
/*
 * Get index of highest nonzero bit
 */
static int
highbit(ulong_t i)
{
	register int h = 1;

	if (i == 0)
		return (0);
#ifdef _LP64
	if (i & 0xffffffff00000000ul) {
		h += 32; i >>= 32;
	}
#endif
	if (i & 0xffff0000) {
		h += 16; i >>= 16;
	}
	if (i & 0xff00) {
		h += 8; i >>= 8;
	}
	if (i & 0xf0) {
		h += 4; i >>= 4;
	}
	if (i & 0xc) {
		h += 2; i >>= 2;
	}
	if (i & 0x2) {
		h += 1;
	}
	return (h);
}
#endif

/*
 * Free a previously allocated id_tree_t.
 */
static void
id_tree_free(id_tree_t *itp)
{
#ifdef _KERNEL
	kmem_free(itp, sizeof (id_tree_t));
#else
	umem_free(itp, sizeof (id_tree_t));
#endif
}

/*
 * Free a previously allocated id_space_t.
 */
static void
id_space_free(id_space_t *isp)
{
#ifdef _KERNEL
	kmem_free(isp, sizeof (id_space_t));
#else
	umem_free(isp, sizeof (id_space_t));
#endif
}

/*
 * Allocate and initialize an id_node_t.
 */
static id_node_t *
id_node_create(boolean_t sleep)
{
	id_node_t *np;

#ifdef _KERNEL
	int flag = sleep ? KM_SLEEP : KM_NOSLEEP;
	np = kmem_alloc(sizeof (id_node_t), flag);
#else
	(void) sleep;
	np = umem_alloc(sizeof (id_node_t), UMEM_DEFAULT);
#endif
	if (np == NULL) {
		return (NULL);
	}
	bzero(np, sizeof (id_node_t));
	return (np);
}

/*
 * Destroy an id_node_t previously allocated with id_node_create. Recursively
 * destroys child nodes.
 */
static void
id_node_destroy(id_node_t *np)
{
	uint_t		i;
	id_node_t	*childp;

	for (i = 0; i < ID_BRANCH_FACTOR; i++) {
		if ((childp = np->idn_children[i]) != NULL) {
			id_node_destroy(childp);
		}
	}
#ifdef _KERNEL
	kmem_free(np, sizeof (id_node_t));
#else
	umem_free(np, sizeof (id_node_t));
#endif
}

/*
 * Allocate and initialize an id_tree_t.
 */
static id_tree_t *
id_tree_create(boolean_t sleep, id_t low, id_t high)
{
	id_tree_t *itp;

#ifdef _KERNEL
	int flag = sleep ? KM_SLEEP : KM_NOSLEEP;
	itp = kmem_alloc(sizeof (id_tree_t), flag);
#else
	(void) sleep;
	itp = umem_alloc(sizeof (id_tree_t), UMEM_DEFAULT);
#endif
	if (itp == NULL) {
		return (NULL);
	}
	if ((itp->idt_root = id_node_create(sleep)) == NULL) {
		id_tree_free(itp);
		return (NULL);
	}
	itp->idt_height	= 0;
	itp->idt_offset = low;
	itp->idt_size = 0;
	/*
	 * Note that max IS NOT the highest ID contained in the tree - it is the
	 * maximum number of IDs the tree contains.
	 */
	itp->idt_max = high - low;
	return (itp);
}

/*
 * Destroy an id_tree_t previously allocated with id_tree_create. Kicks off
 * destruction of all nodes by destroying root.
 */
static void
id_tree_destroy(id_tree_t *tp)
{
	id_node_destroy(tp->idt_root);
	id_tree_free(tp);
}

/*
 * Comparator for use in id_space_t's AVL tree of id_tree_t structs. A tree with
 * a lower offset (e.g. lower starting ID) is "less" than a tree with a higher
 * offset.
 */
static int
id_tree_compare(const void *first, const void *second)
{
	const id_tree_t	*tree1 = first;
	const id_tree_t	*tree2 = second;

	if (tree1->idt_offset < tree2->idt_offset) {
		return (-1);
	} else if (tree1->idt_offset > tree2->idt_offset) {
		return (-1);
	}

	return (0);
}

/*
 * Create an arena to represent the range [low, high).
 * Caller must be in a context in which KM_SLEEP is legal,
 * for the kernel. Always KM_NOSLEEP in userland.
 */
id_space_t *
id_space_create(const char *name, id_t low, id_t high)
{
	id_space_t	*isp;
	id_tree_t	*itp;

	ASSERT(low >= 0);
	ASSERT(low < high);
	ASSERT(high <= MAXUID + 1);

#ifdef _KERNEL
	isp = kmem_alloc(sizeof (id_space_t), KM_SLEEP);
	itp = id_tree_create(B_TRUE, low, high);
#else
	if ((isp = umem_alloc(sizeof (id_space_t), UMEM_DEFAULT)) == NULL) {
		return (NULL);
	}
	if ((itp = id_tree_create(B_FALSE, low, high)) == NULL) {
		id_space_free(isp);
		return (NULL);
	}
#endif

	/* Create AVL tree and add id_tree_t */
	avl_create(&isp->id_trees, id_tree_compare, sizeof (id_tree_t),
	    offsetof(id_tree_t, idt_avl));
	avl_add(&isp->id_trees, itp);

	(void) snprintf(isp->id_name, ID_NAMELEN, "%s", name);
	isp->id_high = high;
	isp->id_low = low;
	isp->id_next_free = low;

	/* Initialize mutexes and condvar as appropriate */
#ifdef _KERNEL
	mutex_init(&isp->id_lock, NULL, MUTEX_DEFAULT, NULL);
	cv_init(&isp->id_cond, NULL, CV_DEFAULT, NULL);
#else
	(void) mutex_init(&isp->id_lock, USYNC_THREAD | LOCK_ERRORCHECK, NULL);
#endif
	return (isp);
}

#ifdef _KERNEL
/*
 * Create an arena to represent the range [low, high). Do not sleep.
 */
id_space_t *
id_space_create_nosleep(const char *name, id_t low, id_t high)
{
	id_space_t	*isp;
	id_tree_t	*itp;

	ASSERT(low >= 0);
	ASSERT(low < high);
	ASSERT(high <= MAXUID + 1);

	isp = kmem_alloc(sizeof (id_space_t), KM_NOSLEEP);
	if (isp == NULL) {
		return (NULL);
	}
	if ((itp = id_tree_create(B_FALSE, low, high)) == NULL) {
		id_space_free(isp);
		return (NULL);
	}

	/* Create AVL tree and add id_tree_t */
	avl_create(&isp->id_trees, id_tree_compare, sizeof (id_tree_t),
	    offsetof(id_tree_t, idt_avl));
	avl_add(&isp->id_trees, itp);

	(void) snprintf(isp->id_name, ID_NAMELEN, "%s", name);
	isp->id_high = high;
	isp->id_low = low;
	isp->id_next_free = low;

	/* Initialize mutexes and condvar as appropriate */
#ifdef _KERNEL
	mutex_init(&isp->id_lock, NULL, MUTEX_DEFAULT, NULL);
	cv_init(&isp->id_cond, NULL, CV_DEFAULT, NULL);
#else
	(void) mutex_init(&isp->id_lock, USYNC_THREAD | LOCK_ERRORCHECK, NULL);
#endif
	return (isp);
}
#endif

/*
 * Destroy a previously created ID space.
 * No restrictions on caller's context.
 */
void
id_space_destroy(id_space_t *isp)
{
	id_tree_t	*itp;
	void		*c = NULL;

	/* Destroy all id_tree_t structs in AVL tree */
	while ((itp = avl_destroy_nodes(&isp->id_trees, &c)) != NULL) {
		id_tree_destroy(itp);
	}
	avl_destroy(&isp->id_trees);
#ifdef _KERNEL
	mutex_destroy(&isp->id_lock);
	cv_destroy(&isp->id_cond);
#else
	(void) mutex_destroy(&isp->id_lock);
#endif
	id_space_free(isp);
}

/*
 * Add the specified range of IDs to an existing ID space. The new range is
 * allowed to overlap arbitrarily with existing ranges. This significantly
 * complicates the logic required of this function.
 *
 * As we iterate through the existing ID ranges, the new ID range will fall into
 * one of four cases, as illustrated below:
 *
 * key:
 *          [             ]   Existing ID range
 *        *******             Range to be added
 *
 * Case 1:  [   ******    ]
 * The range to be added lies completely inside the existing range. We do
 * nothing.
 *
 * Case 2:  [      *******]**
 * The new range overlaps the end of the old range. We truncate the beginning of
 * the new range.
 *
 * Case 3: *[*****        ]
 * The new range overlaps the beginning of the old range. We truncate the
 * end of the new range.
 *
 * Case 4: *[*************]**
 * The new range completely contains the old range. We split the new range into
 * two smaller ranges on either side of the old range.
 *
 * The structure, documentation and ASCII art for this function are shamelessly
 * lifted from the virtual memory implementation in Brown University's Weenix.
 */
void
id_space_extend(id_space_t *isp, id_t low, id_t high)
{
	id_t		adjusted_low = low;
	id_t		adjusted_high = high;
	id_tree_t	*itp;
	id_tree_t	*new_tree;

	ASSERT(isp != NULL);
	ASSERT(low >= 0);
	ASSERT(high <= MAXUID + 1);

	mutex_enter(&isp->id_lock);
	/* Iterate through all existing id_tree_t structs in the AVL tree */
	for (itp = avl_first(&isp->id_trees); itp != NULL;
	    itp = AVL_NEXT(&isp->id_trees, itp)) {

		/* Get ID range of current tree */
		id_t itp_low = itp->idt_offset;
		id_t itp_high = itp->idt_max + itp_low;

		/* We're past the new range */
		if (adjusted_high <= itp_low) {
			break;
		}

		/* We haven't reached the new range */
		if (adjusted_low >= itp_high) {
			continue;
		}

		/*
		 * Start of new range is less than start of existing range, but
		 * end of new range is not less than start of existing range
		 * --> new range overlaps in SOME way
		 */
		if (adjusted_low < itp_low) {
			if (adjusted_high > itp_high) {
				/*
				 * Case 4: old range completely covered - split
				 * new range, allocate lower part, and keep
				 * iterating through trees
				 */
				new_tree = id_tree_create(B_TRUE, adjusted_low,
				    itp_low);
				avl_add(&isp->id_trees, new_tree);
				if (adjusted_low < isp->id_low) {
					isp->id_low = adjusted_low;
				}
				adjusted_low = itp_high;
			} else {
				/* Case 3: truncate end of new range */
				adjusted_high = itp_low;
			}
		} else if (adjusted_high > itp_high) {
			/* Case 2: truncate beginning of new range */
			adjusted_low = itp_high;
		} else {
			/*
			 * Case 1: new range completely encompassed by old
			 * range - do nothing
			 */
			mutex_exit(&isp->id_lock);
			return;
		}
	}

	/* Allocate the free range and update space's high and low */
	new_tree = id_tree_create(B_TRUE, adjusted_low, adjusted_high);
	avl_add(&isp->id_trees, new_tree);
	if (adjusted_high > isp->id_high) {
		isp->id_high = adjusted_high;
	}
	if (adjusted_low < isp->id_low) {
		isp->id_low = adjusted_low;
	}
	mutex_exit(&isp->id_lock);
}

/*
 * Get index of first 0 in bitfield between [low, high). Returns index in slotp
 * pointer. Returns B_FALSE if no suitable index exists, B_TRUE otherwise.
 */
static boolean_t
get_open_slot(ulong_t *bitfield, uint32_t low, uint32_t high, uint32_t *slotp)
{
	uint32_t 	long_bits = sizeof (ulong_t) * 8;
	/* Number of longs in bitfield */
	uint32_t	arr_len = ID_BRANCH_FACTOR / long_bits;
	uint32_t	i;
	/* Offsets of low and high relative to a single long */
	uint32_t	low_offset = low % long_bits;
	uint32_t	high_offset = high % long_bits;
	/* Indices of longs for which masks must be applied */
	uint32_t	low_mask_idx = low / long_bits;
	uint32_t	high_mask_idx = high / long_bits;

	ASSERT(low < high);

	for (i = low/long_bits; i < arr_len; i++) {
		/* Masks for range outside of [low, high) */
		ulong_t low_mask;
		ulong_t high_mask;
		/* Low mask is irrelevant - we're past it */
		if (i > low_mask_idx) {
			low_mask = 0;
		/* Mask out start of long */
		} else {
			/* Avoid shifting by long_bits - this is undefined */
			low_mask = (low_offset == 0) ? 0
			    : -1L << (long_bits - low_offset);
		}

		/* High mask is irrelevant - we haven't hit it yet */
		if (i < high_mask_idx) {
			high_mask = 0;
		/* We haven't hit the valid range yet - keep going */
		} else if (i > high_mask_idx) {
			continue;
		/* Mask out end of long */
		} else {
			/* Avoid shifting by long_bits - this is undefined */
			high_mask = (high_offset == 0) ? 0
			    : (1L << (long_bits - (high % long_bits))) - 1;
		}

		/* Get index of first bit that is a 0, after applying mask */
		uint32_t result = long_bits
		    - highbit(~(bitfield[i] | low_mask | high_mask));

		/*
		 * If the first open index == long_bits, there _is_ no open
		 * index, so we continue. Otherwise, we return the result,
		 * adding i * long_bits to account for where we're at in
		 * the array of longs.
		 */
		if (result < long_bits) {
			ASSERT((i * long_bits) + result >= low);
			if (slotp != NULL) {
				*slotp = (i * long_bits) + result;
			}
			return (B_TRUE);
		}
	}
	return (B_FALSE);
}

/*
 * Sets bit in bitfield to value passed in - 0 or 1.
 */
static void
set_bit(ulong_t *bitfield, uint32_t index, uint32_t value)
{
	uint32_t	long_bits = sizeof (ulong_t) * 8;
	ulong_t 	mask = 1L << (long_bits - (index % long_bits) - 1);

	ASSERT(index < ID_BRANCH_FACTOR);
	ASSERT((value == 0) || (value == 1));

	if (value) {
		bitfield[index / long_bits] |= mask;
	} else {
		bitfield[index / long_bits] &= ~mask;
	}
}

/*
 * Grows height of tree by one level if tree is currently full but has not
 * yet reached max number of IDs it is allowed to hold. Returns B_FALSE if the
 * tree has reached its maximum size or cannot be expanded, B_TRUE otherwise.
 */
static boolean_t
expand_tree(id_tree_t *itp, boolean_t sleep)
{
	/* We're not allowed to make the tree bigger */
	if (itp->idt_size == itp->idt_max) {
		return (B_FALSE);
	}

	/*
	 * tree is currently full - note that this doesn't cover the case where
	 * the tree is at its maximum size but the root's bitfield isn't
	 * totally full - this is handled by the "if" statement above.
	 */
	if (!get_open_slot(itp->idt_root->idn_bitfield, 0, ID_BRANCH_FACTOR,
	    NULL)) {
		id_node_t *new_root = id_node_create(sleep);
		/*
		 * Make a new root and make the current root the first child of
		 * the new root.
		 */
		if (new_root == NULL) {
			return (B_FALSE);
		}
		new_root->idn_children[0] = itp->idt_root;
		set_bit(new_root->idn_bitfield, 0, 1);
		itp->idt_root = new_root;
		itp->idt_height++;
		return (B_TRUE);
	}

	/* We didn't do anything, but that's ok */
	return (B_TRUE);
}

/*
 * This is where most of the magic happens. Walks the given tree looking for an
 * ID to allocate or free. Note that the wrap-around behavior DOES NOT occur
 * in this function - this is because this function only involves a single
 * id_tree_t, and we must wrap around ALL id_tree_t structs in the AVL tree.
 * Returns B_TRUE on success, B_FALSE otherwise.
 *
 * Arguments:
 *	itp:	The tree to walk.
 *	start:	The inded of the first ID to consider. For id_alloc, this will
 *		be continuously incremented, and for id_allocff, it will be 0.
 *	exact:	Whether or not we're looking for the _exact_ id passed in, or
 *		just an ID larger than the id passed in. Should be B_TRUE for
 *		id_alloc_specific and id_free and B_FALSE for everything else.
 *	alloc:	Whether we're allocating or freeing an ID
 *	sleep:	Specifies whether we sleep or fail if we can't alloc or free an
 *		ID.
 *	idp:	The found ID is returned in this pointer, if we're allocating.
 */
static boolean_t
id_tree_walk(id_tree_t *itp, id_t start, boolean_t exact, boolean_t alloc,
    boolean_t sleep, id_t *idp)
{
	/* Variables for tree traversal */
	uint32_t	idx = start;
	uint32_t	scale_power;
	uint32_t	tree_levels;
	uint32_t 	i;
	id_node_t	*curr_node;
	id_node_t	*new_node;
	/* Variables to keep track of path we've taken */
	id_node_t	*path_nodes[ID_MAX_HEIGHT];
	uint32_t	path_slots[ID_MAX_HEIGHT];
	boolean_t	backtrack = B_FALSE;
	/* Variables for maintaining next bitfield slot to look for */
	uint32_t	slot;
	uint32_t	maxslot;
	id_t		result = 0;

	/* Add a layer to the tree if we need to */
	if (alloc && !expand_tree(itp, sleep)) {
		return (B_FALSE);
	}

	curr_node = itp->idt_root;
	tree_levels = itp->idt_height + 1;

	/* scale_power == ID_BRANCH_FACTOR ^ tree_levels */
	scale_power = 1 << (ID_BRANCH_SHIFT * tree_levels);

	/* Iterate down tree */
	for (i = 0; i < tree_levels; i++) {
		/*
		 * idx is the position of the id we're looking for in the
		 * subtree whose root is curr_node. curr_node's subtree
		 * is responsible for scale_power leaves (before scale_power is
		 * divided by ID_BRANCH_FACTOR).
		 */
		idx %= scale_power;
		scale_power /= ID_BRANCH_FACTOR;

		/*
		 * maxslot puts an upper limit on any bitfields that should
		 * never be fully filled because the size of the ID space is
		 * in-between tree sizes. It will only ever be relevant at the
		 * right edge of the tree.
		 */
		maxslot = min(((itp->idt_max - 1 - result) / scale_power) + 1,
		    ID_BRANCH_FACTOR);
		/* Try to take exact path down tree, return -1 if we can't */
		if (exact) {
			slot = idx / scale_power;
			if (alloc) {
				uint32_t openslot;
				boolean_t success =
				    get_open_slot(curr_node->idn_bitfield, slot,
				    maxslot, &openslot);
				if (!success || (openslot != slot)) {
					return (B_FALSE);
				}
			}
		/* Get next open slot */
		} else {
			ASSERT(alloc); /* Inexact frees are not permitted */
			boolean_t success =
			    get_open_slot(curr_node->idn_bitfield,
			    idx / scale_power, maxslot, &slot);
			if (success == B_FALSE) {
				/*
				 * If we get here, it means we've reached a leaf
				 * that DOES have open slots, but the open slots
				 * are all smaller than the "start" ID. This
				 * means we should check the next leaf over in
				 * the tree. We do this recursively.
				 *
				 * This case should be fairly rare.
				 */
				id_t next_start = start + ID_BRANCH_FACTOR -
				    (start % ID_BRANCH_FACTOR);
				if (next_start > itp->idt_max) {
					return (B_FALSE);
				}
				return id_tree_walk(itp, next_start, exact,
				    alloc, sleep, idp);
			}
			/*
			 * If the next open slot is larger than the intended
			 * slot (we didn't get the exact ID we wanted), we
			 * update idx to correspond to the actual slot that we
			 * got - this becomes important when idx is scaled on
			 * the next iteration of the loop.
			 */
			if (slot > idx/scale_power) {
				idx = slot * scale_power;
			}
		}

		/* Fill in our path bookkeeping in case we need to backtrack */
		path_nodes[i] = curr_node;
		path_slots[i] = slot;
		/* Accumulate our slot in the final result */
		result += slot * scale_power;

		/*
		 * If we're not yet at a leaf, we advance curr_node to the
		 * child corresponding to slot. If the child hasn't been
		 * allocated yet, we allocate it.
		 */
		if (i != tree_levels - 1) {
			if (curr_node->idn_children[slot] == NULL) {
				new_node = id_node_create(sleep);
				if (!sleep && new_node == NULL) {
					return (B_FALSE);
				}
				curr_node->idn_children[slot] = new_node;
			}
			curr_node = curr_node->idn_children[slot];
		}
	}

	/* If we're freeing from a fully allocated leaf, we backtrack */
	if (!alloc &&
	    !get_open_slot(curr_node->idn_bitfield, 0, maxslot, NULL)) {
		backtrack = B_TRUE;
	}
	/* Update the bit in the leaf */
	set_bit(curr_node->idn_bitfield, slot, alloc);
	/* If the leaf has become full after we allocated, we backtrack */
	if (alloc &&
	    !get_open_slot(curr_node->idn_bitfield, 0, maxslot, NULL)) {
		backtrack = B_TRUE;
	}

	/* Follow our path back up the tree, updating parents as necessary */
	if (backtrack) {
		for (i = tree_levels - 1; i >= 1; i--) {
			curr_node = path_nodes[i];
			boolean_t child_free =
			    get_open_slot(curr_node->idn_bitfield, 0, maxslot,
			    NULL);
			/*
			 * If we're allocating and the child is now full, or
			 * we're freeing and the child is now not full, set
			 * the bit in the parent accordingly.
			 */
			if (alloc != child_free) {
				slot = path_slots[i-1];
				set_bit(path_nodes[i-1]->idn_bitfield, slot,
				    alloc);
			}
		}
	}

	/* Update tree size */
	if (alloc) {
		itp->idt_size++;
	} else {
		itp->idt_size--;
	}
	ASSERT(result >= start); /* No wrapping here */
	if (alloc && (idp != NULL)) {
		*idp = result;
	}
	return (B_TRUE);
}

/*
 * Called by the user-facing ID space functions.
 * Traverses AVL tree of id_tree_t structs to find suitable tree for ID
 * allocation, then calls id_walk_tree to allocate an ID. This function is where
 * the wrap-around from high to low happens. Returns B_TRUE on success, B_FALSE
 * otherwise.
 *
 * Arguments:
 *	isp:	The ID space to allocate from.
 *	start:	The inded of the first ID to consider. For id_alloc, this will
 *		be continuously incremented, and for id_allocff, it will be 0.
 *	exact:	Whether or not we're looking for the _exact_ id passed in, or
 *		just an ID larger than the id passed in. Should be B_TRUE for
 *		id_alloc_specific and id_free and B_FALSE for everything else.
 *		If it's B_FALSE, we'll automatically wrap around to the
 *		beginning of the AVL tree if there are no IDs larger than start
 * 		that meet our criteria.
 *	alloc:	Whether we're allocating or freeing an ID
 *	sleep:	Specifies whether we sleep or fail if we can't alloc or free an
 *		ID.
 *	idp:	The found ID is returned in this pointer, if we're allocating.
 */
static boolean_t
id_get_id(id_space_t *isp, id_t start, id_t exact, boolean_t alloc,
    boolean_t sleep, id_t *idp)
{
	id_tree_t	*itp;
	uint32_t	i;
	uint32_t	start_tree; /* position of first viable tree */
	id_t		id;

	/* Find first candidate tree */
	for (itp = avl_first(&isp->id_trees), i = 0; itp != NULL;
	    itp = AVL_NEXT(&isp->id_trees, itp), i++) {

		start_tree = i;
		/* This tree has a suitable range - try to allocate an ID */
		if (itp->idt_offset <= start &&
		    (itp->idt_max + itp->idt_offset) > start) {
			/* If this tree is full, move to the next one */
			if (!id_tree_walk(itp, start - itp->idt_offset, exact,
			    alloc, sleep, &id)) {
				break;
			}
			/* We found an id! */
			if (idp != NULL) {
				*idp = id + itp->idt_offset;
			}
			return (B_TRUE);
		}
		/*
		 * We've gone too far so we should move to the next loop because
		 * everything is fair game now
		 */
		if (itp->idt_offset > start) {
			break;
		}
	}

	/* Only look at other trees if we're not looking for a specific id */
	if (!exact) {
		/* First loop through the trees after the starting tree */
		for (; itp != NULL; itp = AVL_NEXT(&isp->id_trees, itp)) {
			if (id_tree_walk(itp, 0, B_FALSE, alloc, sleep, &id)) {
				ASSERT(id > start);
				if (idp != NULL) {
					*idp = id + itp->idt_offset;
				}
				return (B_TRUE);
			}
		}

		/*
		 * Then loop through the trees BEFORE the starting tree,
		 * including the starting tree again.
		 */
		for (itp = avl_first(&isp->id_trees), i = 0; i <= start_tree;
		    itp = AVL_NEXT(&isp->id_trees, itp), i++) {
			if (id_tree_walk(itp, 0, B_FALSE, alloc, sleep, &id)) {
				ASSERT(id < start);
				if (idp != NULL) {
					*idp = id + itp->idt_offset;
				}
				return (B_TRUE);
			}
		}
	}
	/* Everything is totally full */
	return (B_FALSE);
}

/*
 * Allocate an id_t from specified ID space.
 * Caller must be in a context in which VM_SLEEP is legal.
 */
id_t
id_alloc(id_space_t *isp)
{
	id_t		result;
	boolean_t	success;

	mutex_enter(&isp->id_lock);
#ifdef _KERNEL
	/* Try to get an ID until we succeed, sleeping in-between attempts */
	while (!(success = id_get_id(isp, isp->id_next_free, B_FALSE, B_TRUE,
	    B_TRUE, &result))) {
		cv_wait(&isp->id_cond, &isp->id_lock);
	}
#else
	success = id_get_id(isp, isp->id_next_free, B_FALSE, B_TRUE, B_TRUE,
	    &result);
#endif
	if (success) {
		isp->id_next_free = result + 1;
		if (isp->id_next_free == isp->id_high) {
			isp->id_next_free = isp->id_low;
		}
	} else {
		result = -1;
	}
	mutex_exit(&isp->id_lock);
	return (result);
}

/*
 * Allocate an id_t from specified ID space.
 * Returns -1 on failure (see module block comments for more information on
 * failure modes).
 */
id_t
id_alloc_nosleep(id_space_t *isp)
{
	id_t result;

	mutex_enter(&isp->id_lock);
	if (id_get_id(isp, isp->id_next_free, B_FALSE, B_TRUE, B_FALSE,
	    &result)) {
		isp->id_next_free = result + 1;
		if (isp->id_next_free == isp->id_high) {
			isp->id_next_free = isp->id_low;
		}
	} else {
		result = -1;
	}
	mutex_exit(&isp->id_lock);
	return (result);
}

/*
 * Allocate an id_t from specified ID space using FIRSTFIT.
 * Caller must be in a context in which VM_SLEEP is legal.
 */
id_t
id_allocff(id_space_t *isp)
{
	id_t		result;
	boolean_t	success;

	mutex_enter(&isp->id_lock);
#ifdef _KERNEL
	/* Try to get an ID until we succeed, sleeping in-between attempts */
	while (!(success = id_get_id(isp, isp->id_low, B_FALSE, B_TRUE, B_TRUE,
	    &result))) {
		cv_wait(&isp->id_cond, &isp->id_lock);
	}
#else
	success = id_get_id(isp, isp->id_low, B_FALSE, B_TRUE, B_TRUE, &result);
#endif
	if (!success) {
		result = -1;
	}
	mutex_exit(&isp->id_lock);
	return (result);
}

/*
 * Allocate an id_t from specified ID space using FIRSTFIT
 * Returns -1 on failure (see module block comments for more information on
 * failure modes).
 */
id_t
id_allocff_nosleep(id_space_t *isp)
{
	id_t result;

	mutex_enter(&isp->id_lock);
	if (!id_get_id(isp, isp->id_next_free, B_FALSE, B_TRUE, B_FALSE,
	    &result)) {
		result = -1;
	}
	mutex_exit(&isp->id_lock);
	return (result);
}

/*
 * Allocate a specific identifier if possible, returning the id if
 * successful, or -1 on failure.
 */
id_t
id_alloc_specific_nosleep(id_space_t *isp, id_t id)
{
	id_t result;

	mutex_enter(&isp->id_lock);
	if (!id_get_id(isp, id, B_TRUE, B_TRUE, B_FALSE, &result)) {
		result = -1;
	}
	mutex_exit(&isp->id_lock);
	return (result);
}

/*
 * Free a previously allocated ID.
 * No restrictions on caller's context.
 */
void
id_free(id_space_t *isp, id_t id)
{
	mutex_enter(&isp->id_lock);
#ifdef _KERNEL
	/* Wake up a single waiting thread */
	cv_signal(&isp->id_cond);
#endif
	(void) id_get_id(isp, id, B_TRUE, B_FALSE, B_FALSE, NULL);
	mutex_exit(&isp->id_lock);
}
