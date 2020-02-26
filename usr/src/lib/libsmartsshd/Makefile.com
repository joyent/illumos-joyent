#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#
# Copyright 2020 Joyent, Inc.
#

LIBRARY=	libsmartsshd.a
VERS=		.1
OBJECTS=	sshd-plugin.o

include ../../Makefile.lib
include	../../Makefile.rootfs

SRCDIR =	../common
SRCS =	$(OBJECTS:%.o=$(SRCDIR)/%.c)

CPPFLAGS +=	-I$(SRCDIR) -D_REENTRANT -D_FILE_OFFSET_BITS=64
LIBS =		$(DYNLIB) $(LINTLIB)
LDLIBS +=	-lc -lmd5

$(LINTLIB) :=	SRCS=	$(SRCDIR)/$(LINTSRC)

.KEEP_STATE:

all:	$(LIBS)

lint:	lintcheck

include ../../Makefile.targ
