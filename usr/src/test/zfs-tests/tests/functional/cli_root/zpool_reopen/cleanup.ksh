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
# Copyright (c) 2016, 2017 by Intel Corporation. All rights reserved.
# Copyright (c) 2017 Open-E, Inc. All Rights Reserved.
# Copyright 2019 Joyent, Inc.
#

. $STF_SUITE/tests/functional/cli_root/zpool_reopen/zpool_reopen.shlib

verify_runnable "global"

cleanup_devices $DISKS

# Unplug the disk and remove scsi_debug module
case "$(uname)" in
Linux)
	for SDDEVICE in $(get_debug_device); do
		remove_disk $SDDEVICE
	done
	unload_scsi_debug
	;;
esac

log_pass
