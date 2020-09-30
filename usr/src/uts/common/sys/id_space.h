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

#ifndef	_ID_SPACE_H
#define	_ID_SPACE_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <sys/param.h>
#include <sys/types.h>
#include <sys/avl.h>

#ifdef _KERNEL
#include <sys/mutex.h>
#include <sys/ksynch.h>
#else
#include <thread.h>
#endif

#define	ID_BRANCH_SHIFT		7
/* branch factor MUST BE a power of 2 greater than sizeof (ulong_t) */
#define	ID_BRANCH_FACTOR	(1 << ID_BRANCH_SHIFT)
#define	ID_MAX_HEIGHT		10U
#define	ID_NAMELEN		30U

typedef struct id_node {
	ulong_t		idn_bitfield[ID_BRANCH_FACTOR/(8 * sizeof (ulong_t))];
	struct id_node	*idn_children[ID_BRANCH_FACTOR];
} id_node_t;

typedef struct id_tree {
	avl_node_t	idt_avl;
	id_node_t	*idt_root;
	size_t		idt_height;
	id_t		idt_offset;
	id_t		idt_max;	/* max size, NOT highest value */
	size_t		idt_size;
} id_tree_t;

typedef struct id_space {
	char 		id_name[ID_NAMELEN];
	avl_tree_t	id_trees;		/* an AVL tree of id_trees */
	id_t		id_next_free;
	id_t		id_high;
	id_t		id_low;

#ifdef _KERNEL
	kmutex_t	id_lock;
	kcondvar_t	id_cond;
#else
	mutex_t		id_lock;
#endif
} id_space_t;

id_space_t *id_space_create(const char *, id_t, id_t);
#ifdef _KERNEL
id_space_t *id_space_create_nosleep(const char *, id_t, id_t);
#endif
void id_space_destroy(id_space_t *);
void id_space_extend(id_space_t *, id_t, id_t);
id_t id_alloc(id_space_t *);
id_t id_alloc_nosleep(id_space_t *);
id_t id_allocff(id_space_t *);
id_t id_allocff_nosleep(id_space_t *);
id_t id_alloc_specific_nosleep(id_space_t *, id_t);
void id_free(id_space_t *, id_t);

#ifdef	__cplusplus
}
#endif

#endif	/* _ID_SPACE_H */
