#!/bin/sh
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

# Copyright 2026 Edgecast Cloud LLC.
#
# Run in an LX zone with GNU as and ld installed:
#   /native/usr/lib/brand/lx/run_auxv_sysinfo.sh
# The second build deliberately changes the expected AT_SYSINFO_EHDR value
# so that the /proc/self/auxv comparison must reject it with status 14.

set -eu

src_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

as --32 -o "$tmp_dir/auxv_sysinfo.o" "$src_dir/auxv_sysinfo.s"
ld -m elf_i386 -o "$tmp_dir/auxv_sysinfo" \
    "$tmp_dir/auxv_sysinfo.o"
set +e
"$tmp_dir/auxv_sysinfo"
status=$?
set -e
if [ "$status" -ne 0 ]; then
    echo "i386 auxv probe: expected exit 0, got $status" >&2
    exit 1
fi

as --32 --defsym LX_TEST_BAD_EHDR=1 -o \
    "$tmp_dir/auxv_sysinfo_bad_ehdr.o" "$src_dir/auxv_sysinfo.s"
ld -m elf_i386 -o "$tmp_dir/auxv_sysinfo_bad_ehdr" \
    "$tmp_dir/auxv_sysinfo_bad_ehdr.o"

set +e
"$tmp_dir/auxv_sysinfo_bad_ehdr"
status=$?
set -e
if [ "$status" -ne 14 ]; then
    echo "bad AT_SYSINFO_EHDR: expected exit 14, got $status" >&2
    exit 1
fi

echo 'i386 auxv syscall and /proc checks passed'
