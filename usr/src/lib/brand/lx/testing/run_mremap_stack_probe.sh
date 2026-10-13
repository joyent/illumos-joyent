#!/bin/sh
#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may not use this file except in compliance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source. A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#
# Copyright 2026 Edgecast Cloud LLC.
#

set -eu

src=$(dirname "$0")/mremap_stack_probe.c
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

musl-gcc -Wall -Wextra -Werror -pthread "$src" -o "$tmpdir/probe"
"$tmpdir/probe"
