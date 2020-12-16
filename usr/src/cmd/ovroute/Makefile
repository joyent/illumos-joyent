#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#

#
# Copyright 2020 Joyent, Inc.
#

PROG= ovroute
OBJS= ovroute.o
SRCS= $(OBJS:%.o=../%.c)

include ../Makefile.cmd
include ../Makefile.ctf

CSTD = $(CSTD_GNU99)
CLEANFILES += $(OBJS)
CPPFLAGS += -D_REENTRANT
CFLAGS += $(CCVERBOSE)
LDLIBS += -lumem -lofmt -ldladm -lsocket
$(NOT_RELEASE_BUILD)CPPFLAGS += -DDEBUG

.KEEP_STATE:

all: $(PROG)

$(PROG): $(OBJS)
	$(LINK64.c) -o $@ $(OBJS) $(LDLIBS)
	$(POST_PROCESS)

clean:
	-$(RM) $(CLEANFILES)

%.o: %.c
	$(COMPILE64.c) $<
	$(POST_PROCESS_O)

clobber: clean
	$(RM) $(PROG)

install: $(PROG) $(ROOTUSRSBINPROG)

FRC:

include ../Makefile.targ
