#! /usr/bin/ksh
#
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
# Copyright 2015, Richard Lowe.
# Copyright 2019 Joyent, Inc.
#

mkdir /tmp/secflags-test.$$
cd /tmp/secflags-test.$$

/usr/bin/psecflags -s aslr -e sleep 100000 &
pid=$!
# Make sure we generate a kernel core we can find
coreadm -p core $pid
enabled=$(/usr/bin/svcprop -p config_params/process_enabled coreadm)
coreadm_restore=""
if [[ "$enabled" = "false" ]]; then
    coreadm_restore="/usr/bin/coreadm -d process"
    coreadm -e process
fi

cleanup() {
    kill $pid >/dev/null 2>&1
    cd /
    rm -fr /tmp/secflags-test.$$

    $coreadm_restore
}

trap cleanup EXIT

sleep 1
## gcore-produced core
gcore $pid >/dev/null

cat > gcore-expected.$$ <<EOF
core 'core.$pid' of $pid:	sleep 100000
	E:	aslr
	I:	aslr
EOF

/usr/bin/psecflags core.${pid} | grep -v '[LU]:' > gcore-output.$$

if ! diff -u gcore-expected.$$ gcore-output.$$; then
    $coreadm_restore
    exit 1;
fi

## kernel-produced core
kill -SEGV $pid
wait $pid >/dev/null 2>&1
$coreadm_restore

cat > core-expected.$$ <<EOF
core 'core' of $pid:	sleep 100000
	E:	aslr
	I:	aslr
EOF

/usr/bin/psecflags core | grep -v '[LU]:' > core-output.$$

if ! diff -u core-expected.$$ core-output.$$; then
    exit 1;
fi

exit 0
