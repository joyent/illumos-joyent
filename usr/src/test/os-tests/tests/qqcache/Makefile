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

include $(SRC)/cmd/Makefile.cmd
include $(SRC)/test/Makefile.com

PROG = qqcache

LOCAL_OBJS = qqcache-test.o
COMMON_OBJS = qqcache.o
COMMON_DIR = $(SRC)/uts/common/qqcache
OBJS = $(LOCAL_OBJS) $(COMMON_OBJS)

ROOTOPTPKG = $(ROOT)/opt/os-tests
TESTDIR = $(ROOTOPTPKG)/tests/qqcache

CMDS = $(PROG:%=$(TESTDIR)/%)
$(CMDS) := FILEMODE = 0555

CSTD = $(CSTD_GNU99)
LDLIBS += -lumem -lcmdutils

all: $(PROG)

install: all $(CMDS)

clobber: clean
	-$(RM) $(PROG) $(OBJS)

clean:
	-$(RM) $(CLEANFILES)

$(CMDS): $(TESTDIR) $(PROG)

$(TESTDIR):
	$(INS.dir)

$(TESTDIR)/%: %
	$(INS.file)

$(PROG): $(OBJS)
	$(LINK.c) -o $@ $(OBJS) $(LDLIBS)

%.o: $(COMMON_DIR)/%.c
	$(COMPILE.c) -o $@ $<
	$(POST_PROCESS_O)
