#!/bin/ksh -p
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
# Copyright (c) 2017 Abhinav Upadhyay <abhinav@NetBSD.org>.
# All rights reserved.
#

##
#
# ASSERTION:
# The probe name in the output for the -n option should not have any
# leading or trailing spaces.
#
# SECTION: dtrace Utility/-n Option;
#
##

if [ $# != 1 ]; then
	echo expected one argument: '<'dtrace-path'>'
	exit 2
fi

dtrace=$1

o1=$($dtrace -c date -n ' syscall::write:entry {@num[probefunc] = count();}' \
	2>&1 > /dev/null)

if [ "$status" -ne 0 ]; then
	echo $tst: dtrace failed
	exit $status
fi

o2=$(echo "$o1" | head -n 1)
echo "$o2"
