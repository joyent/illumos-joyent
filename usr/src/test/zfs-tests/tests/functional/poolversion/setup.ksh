#!/usr/bin/ksh -p
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

#
# Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#

#
# Copyright (c) 2013, 2016 by Delphix. All rights reserved.
# Copyright 2019 Joyent, Inc.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/poolversion/poolversion.cfg

verify_runnable "global"

if [[ ! -d $TESTDIR ]]; then
	log_must mkdir $TESTDIR
fi

# create a version 1 pool
log_must mkfile $MINVDEVSIZE $VERS_FILE_1
log_must zpool create -o version=1 $TESTPOOL $VERS_FILE_1


# create another version 1 pool
log_must mkfile $MINVDEVSIZE $VERS_FILE_2
log_must zpool create -o version=1 $TESTPOOL2 $VERS_FILE_2

log_pass
