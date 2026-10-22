# illumos-joyent

The illumos-joyent repository is downstream of illumos-gate, and is a
component of a SmartOS build. See the README.md in the smartos-live
repository for details on illumos-joyent's use; as well as general advice on
how to file bugs, community, and more.

## Joyent?!

Since Joyent created SmartOS, their name is scattered throughout many
TritonDataCenter repositories and concepts.

## SmartOS Design Choices and How They Affect illumos-joyent

SmartOS was designed to be a hypervisor. Whether those guests are illumos
native zones (`joyent` or `joyent-minimal` zones in SmartOS), emulation-layer
zones  (`lx` zones), or HVMs (Hardware Virtual Machine zones `bhyve` or the
older `kvm`), the guests are where all user work resides.

To aid in its hypervisory task, SmartOS boots a ramdisk root filesystem in
the global zone, where much of it, mainly `/usr`, is mounted read-only as
well. Because of the relatively static nature of the global zone's root
filesystem, a `manifest` file lives at the top-level, and contains the entire
list of what files and directories are delivered to the root filesystem. When
SmartOS maintainers merge from upstream illumos-gate, we need to track
changes in the `usr/src/pkg/manifests` director to make sure, if needed, they
land in the top-level `manifest` file in illumos-joyent.  The contents of
this ramdisk root is known as a Platform Image (PI).

The ramdisk root also means the order of boot-time tasks can change as
compared to a more conventional illumos distribution. Unless overriden in the
loader-to-unix environment variable `SYSPOOL`, the `zones` pool, mounted on
`/zones`, will be loaded relatively early. The `zones` pool also contains
datasets that mount into the global zone's root filesystem. For example, on
SmartOS, `/var` is mounted from the `zones/var` ZFS dataset.

SmartOS boots in one of three ways: as a standalone machine, as a Triton Data
Center compute node, or as a Triton Data Center head node.  There are subtle
differences between the three, and they show up in specific sections of
illumos-joyent that are not in illumos-gate.

SmartOS that boots from a disk will require the illumos loader, and the
required bits for disk-booting are known as a Boot Image.  The Boot Image and
the Platform Image, while built here, are managed in the smartos-live
repository, including by its piadm(8) command.

## List of substantive differences between illumos-joyent and illumos-gate

This list will change over time. It may gain further differences, or it may
lose a difference by either pushing that difference into illumos-gate, or by
removing the difference from illumos-joyent itself.  All three kinds of
change have happened historically, and all three will continue to happen.

Additional changes resulting from changes mentioned in this file are not
cited here. For example, entries to the `exception_lists/` files, entries in
usr/src/pkg/manifests/` files, and more.  Also, some distro-specific branding
items, such as pictures in usr/src/boot, or changes in the boot-banner, are
not cited here either.

Starting recently, some changes have been marked with `XXX SmartOS` to help
code-readers see if the change is exclusive to SmartOS.

### SmartOS zone brands

SmartOS supports the folowing zone brands:

#### joyent, joyent-minimal, and builder brands

These are the native-zone types for SmartOS. They all lofs-mount the
read-only /usr filesystem (as read-only), but the root filesystem in a joyent
zone is writable. The joyent brand is the standard one, the joyent-minimal
brand imports fewer SMF services, and and the builder brand adds permissions
to aid in testing bhyve.  See the joyent(7) man page for more details.

#### bhyve and KVM brands

These are HVM brands. Their sole purpose is to run a hypervisor under the
added protection of running in a zone. To that end, sometimes the hypervisors
need a small amount of change to work in an HVM brand.

#### lx brand

The Linux-emulating "lx" brand was brought back from the OpenSolaris dead by
Joyent-era SmartOS. It also lives in OmniOS. LX brands (re)introduced many
distro-specific features, some of which will be detailed below.

### `manifest` file

The SmartOS Platform Image (PI) contents are enumerated by this file.

Because SmartOS delivers as a ramdisk root, not every IPS package, and often
not every item in a given IPS package, will be delivered.  To that end we
create a comprehensive manifest in the top-level of illumos-joyent.  Any
additions we make exclusively for ourselves will also appear as either new
IPS manifest files, or additions to existing IPS manifest files.

### The pre-population of certain /etc/ files.

Because of the aforementioned ignorance of IPS manifests, SmartOS needs to 

### `boot.manifest` file

The SmartOS Boot Image contents are enumerated by this file.

Similar to `manifest` but for our usr/src/boot bits.

### Makefiles.testarchive

This Makefile is included by smartos-live to generate a .tgz file of tests
under the `tests-tar` target in its top-level Makefile. It contains a list
of test pkg5/p5m manifest files that it uses to generate a file not unlike
the prior two. See OS-7943 for more.

### `altexec`

Some commonly-offered tools expected to be in traditional UNIX search paths
are not in illumos-joyent (e.g. alternate shells). While they are offered in
pkgsrc (`/opt/local` or `/opt/tools`), they are not in the ramdisk
root. `altexec` hardlinks will live in the ramdisk root, and does not allow
an operator to arbitrarily extend the ramdisk root in read-only /usr/.  See
the altexec(8) man page for details.

### OpenSSL consumers use `sunw_` prefixing.

Due to historical reasons spawning from the past lack of stable
long-term-support versions of OpenSSL, and confusion with more modern
versions living in pkgsrc, the OpenSSL in the platform image delivers both
symbol-prefixed and differently-named versions of OpenSSL.  The current
OpenSSL version for the platform is 3.5. Look for `-lsunw_crypto` or
`-lsunw_ssl` changes in Makefiles. The illumos-extra repository delivers
OpenSSL for illumos-joyent usage only, and it does not deliver generic *.so
symbolic links.

### bhyve changes

Because bhyve in SmartOS is run almost always in a bhyve-brand zone, it needs
to perform certain operations that SmartOS expects of its zones.  In
particular signaling that a zone provision is done requires delaying a
privilege drop until the creation of a file occurs.

Also, due to SmartOS customer complaints, and our requirement to have the
global zone communicate with HVM zones over the virtual serial port, the
bhyve uart has a hard rate limit to prevent some guests from getting
overwhelmed.  See OS-8556 for more.

### lx_netlink socket

An extra entry for /etc/sock2path.d/system%2Fkernel exists for lx zones'
AF_NETLINK socket. Like much with LX, it provides just enough to keep
a set of applications from complaining if it does not exist.

### Datalink management for per-zone datalinks 

A big and early change to SmartOS was to introduce per-zone datalink names,
so, for example, every zone has at least `net0` in SmartOS.  Starting with
OS-249, this has introduced a series of changes into dladm & libdladm,
dlmgmtd, dls, dld, and eventually its IP equivalents in ipmgmtd and ipadm.

### Multiple additions to FMA's topology

See the changes in $LIB/fm/topo/modules for multiple additions, including a
slew of IPMI ones.

### Bunyan logging

The ability to emit logs in json that enable parsing from nodejs's bunyan.

### Encrypted kernel dump

OS-7828 introduce an encrypted kernel dump.  It made function-signature
changes to chacha, and in theory COULD be upstreamed, though its direct tie
to chacha is a choice that perhaps should be severed in favor of
algorithm-independence.

### smartos-test

We run tests on SmartOS using the smartos-test script.  The
usr/src/test/smartos-test/README file contains more details about how to use
illumos-gate and illumos-joyent tests on SmartOS.

### CTF

The Compact Type Format tools in illumos-joyent differ slightly in that the
ctf compression uses the native zone root, even in LX zones (/native contains
the illumos native binaries in LX zones).

### nvlist

The SmartOS libnvpair offers two additional functions nvlist_dump_json() and
nvlist_dump_json_free() for JSON output. It is not documented, but perhaps
could be.

### libresolv2_joy

There is a distinct version of libresolv on SmartOS: libresolv2.  Its
background is documented in OS-2115.

### libsmartsshd

It supports a Triton Data Center feature: smartlogin.

### vnd

vnd was implemented to give KVM zones a performant virtual network
interface.  Given KVM is being replaced with BHYVE in practice, this
subsystem will not be upstreamed. There is a libvnd library, plus vndadm(8)
and vndstat(8).

### libzdoor

Pairs with libsmartsshd to support smartlogin.

### nsswitch

Becuase of direct linkage with libresolv2_joy some of the dynamic-linking at
runtime gets reduced.

### SVP (SDC VXLAN Protocol)

Is a resolver in varpd for Triton Data Center (nee SDC). It combines L2 and
L3 resolution for Triton's Fabric Networks implementation. There is an
excellent block comment documenting the protocol in libvarpd_svp.c.

### column(1)

Brought in from BSD, it was deemed important enough to be included in the
platform image.

### ptools

See OS-7934 and OS-3513 for SmartOS additions to the illumos ptools,
including service FMRIs and more process arguments.

Multiple changes to proc(4FS) assist in these enhancements as well.

### Zone-savvier resource controls

Includes ZFS zone-IO throttling, and CPU bursting limits in zones, several
pieces of the kernel are more zones-aware than illumos-gate, to prevent
noisy-neighbor or even denial-of-service.

### hyprlofs

A fast name space virtual file system. It was developed for use in MANTA
version 1's compute jobs.

### inotify

To enable LX zones, and to provide a transitional interface for
Linux-to-illumos porting, SmartOS brought up inotify(7). It is also available
in OmniOS (for LX reasons, of course).  It puts its tendrils into places
outside itself, such as any kernel filesystem implementation.

### ipfilter Cloud Firewall (cfw) Logging support

For Triton's Cloud Firewall Logging, many hooks needed to land in ipfilter.

### SO_ATTACH_FILTER for ICMP sockets

This LX-inspired enhancement, also in OmniOS, allows ICMP programs (like the
Linux ping(8)) to attach BPF filters to ICMP sockets.

### datafilt socket module

Ported in from OmniOS, this implementation of illumos#1403 allows more
efficiency in TCP connection establishment on the server side.

### nfp(4D) driver

Undocumented, but support for some models of nCipher hardware security
modules.

### epoll(7) support

Another LX-required feature that is also available to native illumos
programs. Also in OmniOS.

### dboot kernel startup memory mapping changes

Combinations of difficult hardware along with support for booting from iPXE
inspired changes in dboot (the 32-bit real mode execution before the UNIX
kernel fully takes over).

### vmxnet(4D) driver

Early in the days of SmartOS, this VMware-guest-specific driver was brought
up in SmartOS.  It could be upstreamed in theory.

### In-source driver_aliases, driver_classes, name_to_major, and minor_perm
    driver files.

Similar to `manifest` above, these files' contents are not generated by pkg5
manifest files, but added in usr/src/uts/inetl/os/ files.

