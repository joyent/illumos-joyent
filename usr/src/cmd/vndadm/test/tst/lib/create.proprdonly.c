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
 * Copyright (c) 2014 Joyent, Inc.  All rights reserved.
 */

/*
 * Validate that we can't set read only properties
 */

#include <errno.h>
#include <stdio.h>
#include <strings.h>
#include <assert.h>
#include <limits.h>
#include <libvnd.h>

int
main(int argc, const char *argv[])
{
	int syserr, ret;
	vnd_errno_t vnderr;
	vnd_handle_t *vhp;
	vnd_prop_buf_t vpb;

	if (argc < 2) {
		(void) fprintf(stderr, "missing arguments...\n");
		return (1);
	}

	if (strlen(argv[1]) >= LIBVND_NAMELEN) {
		(void) fprintf(stderr, "vnic name too long...\n");
		return (1);
	}

	vhp = vnd_create(NULL, argv[1], argv[1], &vnderr, &syserr);
	assert(vhp != NULL);
	assert(vnderr == 0);
	assert(syserr == 0);

	ret = vnd_prop_get(vhp, VND_PROP_MINTU, &vpb,
	    sizeof (vnd_prop_buf_t));
	assert(ret == 0);

	ret = vnd_prop_set(vhp, VND_PROP_MINTU, &vpb,
	    sizeof (vnd_prop_buf_t));
	assert(ret == -1);
	assert(vnd_errno(vhp) == VND_E_PROPRDONLY);
	assert(vnd_syserrno(vhp) == 0);

	vnd_close(vhp);

	return (0);
}
