/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may not use this file except in compliance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source. A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * Build with musl-gcc and run in an LX zone. musl uses mremap(2) failures
 * to find the main thread's stack bounds, which Go's cgo startup consumes.
 */

#define	_GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

int
main(void)
{
	pthread_attr_t attr;
	char marker;
	void *base, *addr, *remapped;
	size_t pagesize, size;
	int err, failures = 0;

	pagesize = (size_t)sysconf(_SC_PAGESIZE);
	base = mmap(NULL, 4 * pagesize, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED) {
		perror("mmap");
		return (1);
	}

	errno = 0;
	remapped = mremap((char *)base + pagesize, pagesize,
	    2 * pagesize, 0);
	if (remapped != MAP_FAILED || errno != ENOMEM) {
		(void) fprintf(stderr, "interior mremap: expected ENOMEM, "
		    "got %p, errno %d\n", remapped, errno);
		failures++;
	}

	errno = 0;
	remapped = mremap((char *)base + pagesize, 0, pagesize, 0);
	if (remapped != MAP_FAILED || errno != EINVAL) {
		(void) fprintf(stderr, "zero old size: expected EINVAL, "
		    "got %p, errno %d\n", remapped, errno);
		failures++;
	}
	if (munmap(base, 4 * pagesize) != 0) {
		perror("munmap");
		return (1);
	}

	err = pthread_getattr_np(pthread_self(), &attr);
	if (err != 0) {
		(void) fprintf(stderr, "pthread_getattr_np: %d\n", err);
		return (1);
	}
	err = pthread_attr_getstack(&attr, &addr, &size);
	(void) pthread_attr_destroy(&attr);
	if (err != 0) {
		(void) fprintf(stderr, "pthread_attr_getstack: %d\n", err);
		return (1);
	}
	if (size <= pagesize || (uintptr_t)&marker < (uintptr_t)addr ||
	    (uintptr_t)&marker >= (uintptr_t)addr + size) {
		(void) fprintf(stderr, "bad main stack bounds: [%p, %p), "
		    "size %zu, marker %p\n", addr, (char *)addr + size,
		    size, (void *)&marker);
		failures++;
	}

	if (failures == 0)
		(void) printf("LX musl stack bounds and mremap errno passed\n");
	return (failures != 0);
}
