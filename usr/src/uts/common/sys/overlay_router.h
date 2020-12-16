/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2020 Joyent, Inc.
 */

#ifndef _OVERLAY_ROUTER_H
#define	_OVERLAY_ROUTER_H

/*
 * Overlay device router ioctl interface (/dev/overlay)
 */

#include <sys/types.h>
#include <netinet/in.h>
#include <sys/debug.h>
#include <sys/ethernet.h>
#include <sys/overlay_common.h>
#include <sys/uuid.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct overlay_router_ioc_hdr {
	datalink_id_t	orih_linkid;
	uint32_t	orih_pad[1];
} overlay_router_ioc_hdr_t;
CTASSERT(sizeof (overlay_router_ioc_hdr_t) == sizeof (uint64_t));

/*
 * The ID strings need to be large enough to hold a UUID in string form.
 * Anyone using non-UUIDs is likely to not need more characters than this,
 * so UUID_PRINTABLE_STRING_LENGTH is used as the size (this includes the
 * terminating NUL).
 */
#define	OVERLAY_ID_MAX		UUID_PRINTABLE_STRING_LENGTH

#define	OVERLAY_ROUTER_IOCTL	(('o' << 24) | ('v' << 16) | ('r' << 8))

#define	OVERLAY_ROUTER_NET_CREATE		(OVERLAY_ROUTER_IOCTL | 0x10)
#define	OVERLAY_ROUTER_NET_DELETE		(OVERLAY_ROUTER_IOCTL | 0x11)
#define	OVERLAY_ROUTER_NET_DELETE_ALL		(OVERLAY_ROUTER_IOCTL | 0x12)
#define	OVERLAY_ROUTER_NET_GET			(OVERLAY_ROUTER_IOCTL | 0x13)
#define	OVERLAY_ROUTER_NET_ITER			(OVERLAY_ROUTER_IOCTL | 0x14)
#define	OVERLAY_ROUTER_NET_SET_ROUTETBL		(OVERLAY_ROUTER_IOCTL | 0x15)

typedef struct overlay_ioc_net {
	overlay_router_ioc_hdr_t	oin_hdr;
	struct in6_addr			oin_routeraddrv6;
	in_addr_t			oin_routeraddr;
	uint8_t				oin_prefixlen;
	uint8_t				oin_prefixlenv6;
	uint16_t			oin_vlan;	/* in host order */
	uint8_t				oin_mac[ETHERADDRL];
	char				oin_id[OVERLAY_ID_MAX];
	char				oin_routetbl[OVERLAY_ID_MAX];
} overlay_ioc_net_t;

/* Max # of entries per ioctl() call */
#define	OVERLAY_ROUTER_ITER_MAX	UINT16_MAX

typedef struct overlay_ioc_net_iter {
	overlay_router_ioc_hdr_t	oini_hdr;
	uint64_t			oini_marker;
	uint16_t			oini_count;
	uint8_t				oini_pad2[3];
	overlay_ioc_net_t		oini_ents[];
} overlay_ioc_net_iter_t;

#define	OVERLAY_ROUTETBL_GET		(OVERLAY_ROUTER_IOCTL | 0x20)
#define	OVERLAY_ROUTETBL_SET		(OVERLAY_ROUTER_IOCTL | 0x21)
#define	OVERLAY_ROUTETBL_REMOVE		(OVERLAY_ROUTER_IOCTL | 0x22)
#define	OVERLAY_ROUTETBL_SET_DEFAULT	(OVERLAY_ROUTER_IOCTL | 0x23)
#define	OVERLAY_ROUTETBL_FLUSH		(OVERLAY_ROUTER_IOCTL | 0x24)
#define	OVERLAY_ROUTETBL_ITER		(OVERLAY_ROUTER_IOCTL | 0x25)
#define	OVERLAY_ROUTETBL_ADDENT		(OVERLAY_ROUTER_IOCTL | 0x26)
#define	OVERLAY_ROUTETBL_DELENT		(OVERLAY_ROUTER_IOCTL | 0x27)
#define	OVERLAY_ROUTETBL_FLUSHENT	(OVERLAY_ROUTER_IOCTL | 0x28)

/*
 * We use the same struct for IPv4 and IPv6 targets. We assume route tables
 * should be small, so the small amount of space saved by having separate IPv4
 * and IPv6 entries would be negligible. Since this is an implementation
 * detail, we can always change it later if it proves to be problematic.
 *
 * This is also used for both ioctls and internally within the overlay
 * driver (thus no ioc in the name).
 */
typedef struct overlay_route_ent {
	struct sockaddr_in6	ore_target;	/* the 'next hop' */
	struct in6_addr		ore_dest;
	uint8_t			ore_prefixlen;
} overlay_route_ent_t;

typedef struct overlay_ioc_routetab {
	overlay_router_ioc_hdr_t	oir_hdr;
	uint64_t			oir_marker; /* for entry iteration */
	uint16_t			oir_count;
	char				oir_id[OVERLAY_ID_MAX];
	overlay_route_ent_t		oir_ents[];
} overlay_ioc_routetab_t;

typedef struct overlay_ioc_rtab_iter {
	overlay_router_ioc_hdr_t oiri_hdr;
	uint64_t		oiri_marker; /* for route table iteration */
	uint16_t		oiri_count;
	uint8_t			oiri_pad[6];
	overlay_ioc_routetab_t	oiri_rtabs[];
} overlay_ioc_rtab_iter_t;

#ifdef __cplusplus
}
#endif

#endif /* _OVERLAY_ROUTER_H */
