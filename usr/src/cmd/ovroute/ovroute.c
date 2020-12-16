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

#include <sys/overlay_router.h>
#include <sys/sysmacros.h>
#include <sys/stat.h>
#include <sys/debug.h>
#include <sys/vlan.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include <err.h>
#include <errno.h>
#include <umem.h>
#include <unistd.h>
#include <ofmt.h>
#include <libdladm.h>
#include <libdllink.h>
#include <regex.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/*
 * To facilitate potential future changes, we have an undocumented
 * "_version" subcommand that net-agent can use to determine the cmdline
 * options supported. This should follow semver semantics.
 */
#define	OVROUTE_MAJOR	1
#define	OVROUTE_MINOR	0

typedef enum {
	OVPARAM_NONE =		0x00,
	OVPARAM_DEVICE =	0x01,
	OVPARAM_VLAN =		0x02,
	OVPARAM_ADDRESS_V4 =	0x04,
	OVPARAM_ADDRESS_V6 =	0x08,
	OVPARAM_MAC =		0x10,
	OVPARAM_ROUTETBL =	0x20,
} ovroute_param_t;

extern const char *__progname;

static const char *overlay_dev = "/dev/overlay_router";
static const char *id_re = "[A-Za-z0-9][A-Za-z#.-]*";
static const char *empty = "-";

typedef struct dispatch_tbl {
	const char	*dt_subcmd;
	int		(*dt_cmd)(int, char **);
} dispatch_tbl_t;

typedef enum net_members {
	NM_ID = 0,
	NM_NET,
	NM_NETV6,
	NM_VLAN,
	NM_ROUTERADDR,
	NM_ROUTERADDRV6,
	NM_MAC,
	NM_ROUTETBL
} net_members_t;

static boolean_t net_print(ofmt_arg_t *, char *, uint_t);

static const ofmt_field_t net_fields[] = {
	{
		.of_name = "ID",
		.of_width = OVERLAY_ID_MAX,
		.of_id = NM_ID,
		.of_cb = net_print,
	},
	{
		.of_name = "NETWORK",
		.of_width = INET_ADDRSTRLEN + 3, /* +3 for /prefixlen */
		.of_id = NM_NET,
		.of_cb = net_print,
	},
	{
		.of_name = "NETWORK-V6",
		.of_width = INET6_ADDRSTRLEN + 4, /* +4 for /prefixlen */
		.of_id = NM_NETV6,
		.of_cb = net_print,
	},
	{
		.of_name = "VLAN",
		.of_width = 4,
		.of_id = NM_VLAN,
		.of_cb = net_print,
	},
	{
		.of_name = "ROUTER",
		.of_width = 32,
		.of_id = NM_ROUTERADDR,
		.of_cb = net_print,
	},
	{
		.of_name = "MAC",
		.of_width = ETHERADDRSTRL,
		.of_id = NM_MAC,
		.of_cb = net_print,
	},
	{
		.of_name = "RTABLE",
		.of_width = OVERLAY_ID_MAX,
		.of_id = NM_ROUTETBL,
		.of_cb = net_print,
	},
	{ NULL, 0, 0, NULL }
};

typedef enum routetbl_members {
	RM_ID = 0,
	RM_NENTS
} routetbl_member_t;

static boolean_t routetbl_print(ofmt_arg_t *, char *, uint_t);

static const ofmt_field_t routetbl_fields[] = {
	{
		.of_name = "ID",
		.of_width = OVERLAY_ID_MAX,
		.of_id = RM_ID,
		.of_cb = routetbl_print,
	},
	{
		.of_name = "ENTRIES",
		.of_width = 5,
		.of_id = RM_NENTS,
		.of_cb = routetbl_print,
	},
	{ NULL, 0, 0, NULL }
};

typedef struct routetbl_print {
	char				rpt_id[OVERLAY_ID_MAX];
	const overlay_route_ent_t	*rpt_ent;
} routetbl_print_t;

typedef enum routetbl_ent_memb {
	REM_ID = 0,
	REM_DEST,
	REM_TARGET
} routetbl_ent_memb_t;

static boolean_t routeent_print(ofmt_arg_t *, char *, uint_t);

static const ofmt_field_t routeent_fields[] = {
	{
		.of_name = "ID",
		.of_width = OVERLAY_ID_MAX,
		.of_id = REM_ID,
		.of_cb = routeent_print,
	},
	{
		.of_name = "DESTINATION",
		.of_width = INET6_ADDRSTRLEN + 4,
		.of_id = REM_DEST,
		.of_cb = routeent_print,
	},
	{
		.of_name = "TARGET",
		.of_width = INET6_ADDRSTRLEN,
		.of_id = REM_TARGET,
		.of_cb = routeent_print,
	},
	{ NULL, 0, 0, NULL }
};

static void get_linkid(const char *, datalink_id_t *);
static int open_overlay(const char *, boolean_t);
static int do_ioctl(int, void *, boolean_t);

static int do_help(int, char **);

static int do_version(int, char **);

static int do_router(int, char **);
static int do_router_create(int, char **);
static int do_router_delete(int, char **);
static int do_router_set_routing_table(int, char **);
static int do_router_get(int, char **);

static int do_route_table(int, char **);
static int do_routetbl_create(int, char **);
static int do_routetbl_delete(int, char **);
static int do_routetbl_set_default(int, char **);
static int do_routetbl_get(int, char **);
static int do_routetbl_addent(int, char **);
static int do_routetbl_delent(int, char **);

static void parse_vlan(const char *, uint16_t *);
static void parse_mac(const char *, uint8_t *);
static void parse_id(const char *, char *, size_t);
static void parse_addr(const char *, struct in6_addr *, uint8_t *);
static void parse_addr_port(const char *, struct sockaddr_in6 *);

static dispatch_tbl_t main_tbl[] = {
	{ "_version", do_version },
	{ "router", do_router },
	{ "route-table", do_route_table },
	{ "help", do_help },
};

static dispatch_tbl_t router_tbl[] = {
	{ "create", do_router_create },
	{ "delete", do_router_delete },
	{ "set-routing-table", do_router_set_routing_table },
	{ "get", do_router_get },
	{ "help", do_help },
};

static dispatch_tbl_t route_tbl_tbl[] = {
	{ "create", do_routetbl_create },
	{ "delete", do_routetbl_delete },
	{ "set-default", do_routetbl_set_default },
	{ "get", do_routetbl_get },
	{ "add", do_routetbl_addent },
	{ "del", do_routetbl_delent }
};

static inline void
net_addr(in_addr_t *dest, const in_addr_t *src, uint8_t prefixlen)
{
	const in_addr_t mask = htonl(((in_addr_t)1 << (32 - prefixlen)) - 1);
	*dest = *src & ~mask;
}

static inline void
net_addr6(struct in6_addr *dest, const struct in6_addr *src, uint8_t prefixlen)
{
	struct in6_addr maskv6;

	/* There's probably a better way to do this, but for now... */
	maskv6._S6_un._S6_u32[0] = IN6_MASK_FROM_PREFIX(0, prefixlen);
	maskv6._S6_un._S6_u32[1] = IN6_MASK_FROM_PREFIX(1, prefixlen);
	maskv6._S6_un._S6_u32[2] = IN6_MASK_FROM_PREFIX(2, prefixlen);
	maskv6._S6_un._S6_u32[3] = IN6_MASK_FROM_PREFIX(3, prefixlen);

	for (uint_t i = 0; i < 4; i++) {
		uint32_t sval = ntohl(src->_S6_un._S6_u32[i]);
		uint32_t mask = maskv6._S6_un._S6_u32[i];

		dest->_S6_un._S6_u32[i] = htonl(sval & mask);
	}
}

static void __NORETURN
usage(void)
{
	/* BEGIN CSTYLED */
	(void) fprintf(stderr,
"Usage: %1$s router create -d overlay -m macaddr -v vlan -r route_table \n"
"\t-a address/mask [-a address/mask] router_id\n"
"       %1$s router delete -d overlay router_id\n"
"       %1$s router get -d overlay [router_id...]\n"
"       %1$s router set-routing-table -d overlay -r route_table router_id\n"
"       %1$s route-table create -d overlay routetbl_id\n"
"       %1$s route-table delete -d overlay routetbl_id\n"
"       %1$s route-table get -d overlay [routetbl_id...]\n"
"       %1$s route-table set-default -d overlay routetbl_id\n"
"       %1$s route-table add -d overlay -i routetbl_id destination target\n"
"       %1$s route-table del -d overlay -i routetbl_id destination target\n",
	    __progname);
	/* END CSTYLED */

	exit(2);
}

static int
dispatch(int argc, char **argv, const dispatch_tbl_t *tbl, size_t ntbl)
{
	if (argc < 2) {
		(void) fprintf(stderr, "Missing subcommand\n");
		usage();
	}

	for (size_t i = 0; i < ntbl; i++) {
		if (strcmp(argv[1], tbl[i].dt_subcmd) == 0)
			return (tbl[i].dt_cmd(argc - 1, argv + 1));
	}

	(void) fprintf(stderr, "Unknown subcommand '%s'\n", argv[1]);
	usage();
}

static int
do_help(int argc __unused, char **argv __unused)
{
	usage();
}

static int
do_version(int argc __unused, char **argv __unused)
{
	(void) printf("%d.%d\n", OVROUTE_MAJOR, OVROUTE_MINOR);
	return (0);
}

static int
nomem_cb(void)
{
	(void) fprintf(stderr, "%s: Out of memory\n", __progname);
	abort();
}

int
main(int argc, char **argv)
{
	/* Treat all alloc failures as fatal. */
	umem_nofail_callback(nomem_cb);

	return (dispatch(argc, argv, main_tbl, ARRAY_SIZE(main_tbl)));
}

static int
do_router(int argc, char **argv)
{
	return (dispatch(argc, argv, router_tbl, ARRAY_SIZE(router_tbl)));
}

static int
do_router_create(int argc, char **argv)
{
	const char *ovname = NULL;
	int c, ret;
	overlay_ioc_net_t orn = { 0 };
	ovroute_param_t params = OVPARAM_NONE;
	const ovroute_param_t req_params =
	    (OVPARAM_DEVICE|OVPARAM_VLAN|OVPARAM_MAC);
	struct in6_addr addr;
	uint8_t prefixlen;

	while ((c = getopt(argc, argv, "a:d:m:r:v:")) != -1) {
		switch (c) {
		case 'a':
			parse_addr(optarg, &addr, &prefixlen);
			if (IN6_IS_ADDR_V4MAPPED(&addr)) {
				if ((params & OVPARAM_ADDRESS_V4) != 0) {
					(void) fprintf(stderr, "Can only "
					    "specify one IPv4 address\n");
					usage();
				}
				params |= OVPARAM_ADDRESS_V4;

				IN6_V4MAPPED_TO_IPADDR(&addr,
				    orn.oin_routeraddr);
				orn.oin_prefixlen = 32 - (128 - prefixlen);
			} else {
				if ((params & OVPARAM_ADDRESS_V6) != 0) {
					(void) fprintf(stderr, "Can only "
					    "specify one IPv6 address\n");
					usage();
				}
				params |= OVPARAM_ADDRESS_V6;
				bcopy(&addr, &orn.oin_routeraddrv6,
				    sizeof (addr));
				orn.oin_prefixlenv6 = prefixlen;
			}
			break;
		case 'd':
			ovname = optarg;
			params |= OVPARAM_DEVICE;
			break;
		case 'm':
			parse_mac(optarg, orn.oin_mac);
			params |= OVPARAM_MAC;
			break;
		case 'r':
			parse_id(optarg, orn.oin_routetbl,
			    sizeof (orn.oin_routetbl));
			params |= OVPARAM_ROUTETBL;
			break;
		case 'v':
			parse_vlan(optarg, &orn.oin_vlan);
			params |= OVPARAM_VLAN;
			break;
		case '?':
			(void) fprintf(stderr, "Unknown option -%c\n", optopt);
			usage();
		}
	}

	if (argc < optind)
		errx(EXIT_FAILURE, "Router network id missing");

	parse_id(argv[optind], orn.oin_id, sizeof (orn.oin_id));
	get_linkid(ovname, &orn.oin_hdr.orih_linkid);

	/*
	 * We require at least one address, as well as the parameters in
	 * req_params.
	 *
	 * XXX: Error message could probably be made better.
	 */
	if ((params & req_params) != req_params ||
	    (params & (OVPARAM_ADDRESS_V4|OVPARAM_ADDRESS_V6)) == 0)
		errx(EXIT_FAILURE, "required parameters missing");

	ret = do_ioctl(OVERLAY_ROUTER_NET_CREATE, &orn, B_FALSE);
	if (ret != 0) {
		err(EXIT_FAILURE, "failed to create router net %s on %s",
		    orn.oin_id, ovname);
	}

	return (0);
}

static int
do_router_delete(int argc, char **argv)
{
	const char *ovname = NULL;
	int c, ret;
	overlay_ioc_net_t orn = { 0 };

	while ((c = getopt(argc, argv, "d:")) != -1) {
		switch (c) {
		case 'd':
			ovname = optarg;
			break;
		}
	}

	if (argc < optind) {
		(void) fprintf(stderr, "Router network id missing\n");
		usage();
	}

	if (ovname == NULL) {
		(void) fprintf(stderr, "Overlay name missing\n");
		usage();
	}

	parse_id(argv[optind], orn.oin_id, sizeof (orn.oin_id));
	get_linkid(ovname, &orn.oin_hdr.orih_linkid);

	ret = do_ioctl(OVERLAY_ROUTER_NET_DELETE, &orn, B_FALSE);
	if (ret != 0) {
		err(EXIT_FAILURE, "failed to delete router net %s",
		    argv[1]);
	}

	return (0);
}

static int
do_router_set_routing_table(int argc, char **argv)
{
	const char *ovname = NULL;
	overlay_ioc_net_t orn = { 0 };
	ovroute_param_t params = OVPARAM_NONE;
	int c, ret;

	while ((c = getopt(argc, argv, "d:r:")) != -1) {
		switch (c) {
		case 'd':
			ovname = optarg;
			params |= OVPARAM_DEVICE;
			break;
		case 'r':
			parse_id(optarg, orn.oin_routetbl,
			    sizeof (orn.oin_routetbl));
			params |= OVPARAM_ROUTETBL;
			break;
		}
	}

	if ((params & (OVPARAM_DEVICE|OVPARAM_ROUTETBL)) == 0) {
		(void) fprintf(stderr,
		    "Missing overlay device or route table id\n");
		usage();
	}

	if (argc < optind) {
		(void) fprintf(stderr, "Router network id missing\n");
		usage();
	}

	parse_id(argv[optind], orn.oin_id, sizeof (orn.oin_id));
	get_linkid(ovname, &orn.oin_hdr.orih_linkid);

	ret = do_ioctl(OVERLAY_ROUTER_NET_SET_ROUTETBL, &orn, B_FALSE);
	if (ret != 0) {
		err(EXIT_FAILURE, "failed to set route table");
	}

	return (0);
}

static boolean_t
netaddr_print(int af, const void *addr, char *buf, size_t buflen)
{
	if (af == AF_INET) {
		if (af == INADDR_ANY) {
			(void) strlcpy(buf, empty, buflen);
			return (B_FALSE);
		}
	} else if (af == AF_INET6) {
		const struct in6_addr *a6 = addr;
		if (IN6_IS_ADDR_UNSPECIFIED(a6)) {
			(void) strlcpy(buf, empty, buflen);
			return (B_FALSE);
		}
	}

	(void) inet_ntop(af, addr, buf, buflen);
	return (B_TRUE);
}

static void
netprefix_print(int af, const void *addr, uint8_t prefixlen, char *buf,
    size_t buflen)
{
	if (!netaddr_print(af, addr, buf, buflen))
		return;

	char prefixbuf[5];

	(void) snprintf(prefixbuf, sizeof (prefixbuf), "/%" PRIu8, prefixlen);
	(void) strlcat(buf, prefixbuf, buflen);
}

static void
sockaddr_print(const struct sockaddr_in6 *addr, char *buf, size_t buflen)
{
	const void *addrp;
	in_addr_t v4;
	int af;

	if (IN6_IS_ADDR_V4MAPPED(&addr->sin6_addr)) {
		af = AF_INET;
		IN6_V4MAPPED_TO_IPADDR(&addr->sin6_addr, v4);
		addrp = &v4;
	} else {
		af = AF_INET6;
		addrp = addr;
	}

	if (!netaddr_print(af, addrp, buf, buflen) || addr->sin6_port == 0)
		return;

	char portstr[7]; /* ':' + 5 digit port */

	(void) snprintf(portstr, sizeof (portstr), ":%hhu",
	    ntohs(addr->sin6_port));
	(void) strlcat(buf, portstr, buflen);
}

static boolean_t
net_print(ofmt_arg_t *ofmt, char *buf, uint_t buflen)
{
	overlay_ioc_net_t *net = ofmt->ofmt_cbarg;
	in_addr_t addr;
	struct in6_addr addr6;

	/*
	 * ofmt_id is cast to net_members_t so any additional fields
	 * added in the future will trigger a compile error if not
	 * handled here.
	 */
	switch ((net_members_t)ofmt->ofmt_id) {
	case NM_ID:
		(void) strlcpy(buf, net->oin_id, buflen);
		break;
	case NM_NET:
		net_addr(&addr, &net->oin_routeraddr, net->oin_prefixlen);
		netprefix_print(AF_INET, &addr, net->oin_prefixlen, buf,
		    buflen);
		break;
	case NM_NETV6:
		net_addr6(&addr6, &net->oin_routeraddrv6, net->oin_prefixlenv6);
		netprefix_print(AF_INET6, &addr6, net->oin_prefixlenv6, buf,
		    buflen);
		break;
	case NM_VLAN:
		(void) snprintf(buf, buflen, "%" PRIu16, net->oin_vlan);
		break;
	case NM_ROUTERADDR:
		(void) netaddr_print(AF_INET, &net->oin_routeraddr, buf,
		    buflen);
		break;
	case NM_ROUTERADDRV6:
		(void) netaddr_print(AF_INET6, &net->oin_routeraddrv6, buf,
		    buflen);
		break;
	case NM_MAC:
		(void) strlcpy(buf,
		    ether_ntoa((const struct ether_addr *)net->oin_mac),
		    buflen);
		break;
	case NM_ROUTETBL:
		if (strlen(net->oin_routetbl) == 0)
			(void) strlcpy(buf, empty, buflen);
		else
			(void) strlcpy(buf, net->oin_routetbl, buflen);
		break;
	}

	return (B_TRUE);
}

static size_t
router_net_iter_size(size_t nent)
{
	VERIFY3U(nent, <=, OVERLAY_ROUTER_ITER_MAX);

	size_t sz = sizeof (overlay_ioc_net_iter_t);
	size_t entsz = nent * sizeof (overlay_ioc_net_t);
	size_t tot = 0;

	/* This shouldn't happen, but force a core if it does */
	if (nent != 0 && entsz < nent)
		goto overflow;

	tot = sz + entsz;
	if (tot < sz)
		goto overflow;

	return (tot);

overflow:
	(void) fprintf(stderr, "%s: nent = %zu caused overflow "
	    "(entsz = %zu, tot = %zu)\n", __func__, nent, entsz, tot);
	abort();
}

static overlay_ioc_net_iter_t *
net_iter_alloc(const char *ovname, size_t nents)
{
	overlay_ioc_net_iter_t *iter;

	iter = umem_zalloc(router_net_iter_size(nents), UMEM_NOFAIL);

	get_linkid(ovname, &iter->oini_hdr.orih_linkid);
	bzero(iter->oini_ents, nents * sizeof (overlay_ioc_net_t));
	iter->oini_count = nents;
	return (iter);
}

static void
net_iter_free(overlay_ioc_net_iter_t *iter, size_t nents)
{
	if (iter == NULL)
		return;
	umem_free(iter, router_net_iter_size(nents));
}

static int
do_router_iter(const char *ovname, const char *ofields, uint_t flags)
{
	const size_t nents = 64;
	overlay_ioc_net_iter_t *iter = net_iter_alloc(ovname, nents);
	ofmt_handle_t ofmt = NULL;
	ofmt_status_t oferr;
	int fd = open_overlay(overlay_dev, B_TRUE);
	int ret;

	oferr = ofmt_open(ofields, net_fields, flags, 0, &ofmt);
	if (oferr != OFMT_SUCCESS) {
		char ebuf[OFMT_BUFSIZE];

		errx(EXIT_FAILURE, "%s: ofmt_open failed: %s", __func__,
		    ofmt_strerror(ofmt, oferr, ebuf, sizeof (ebuf)));
	}

	for (;;) {
		ret = ioctl(fd, OVERLAY_ROUTER_NET_ITER, iter);

		if (ret != 0)
			err(EXIT_FAILURE, "error iterating router nets");

		if (iter->oini_count == 0)
			break;

		for (uint_t i = 0; i < iter->oini_count; i++)
			ofmt_print(ofmt, &iter->oini_ents[i]);

		bzero(iter->oini_ents, nents * sizeof (overlay_ioc_net_t));
		iter->oini_count = nents;
	}

	ofmt_close(ofmt);
	VERIFY0(close(fd));
	net_iter_free(iter, nents);
	return (0);
}

static int
do_router_get(int argc, char **argv)
{
	const char *ovname = NULL;
	const char *ofields = "all";
	uint_t flags = 0;
	int c;

	while ((c = getopt(argc, argv, "d:o:p")) != -1) {
		switch (c) {
		case 'd':
			ovname = optarg;
			break;
		case 'o':
			ofields = optarg;
			break;
		case 'p':
			flags |= OFMT_PARSABLE;
			break;
		}
	}

	if (ovname == NULL) {
		(void) fprintf(stderr, "Missing overlay device name\n");
		usage();
	}

	if (optind == argc)
		return (do_router_iter(ovname, ofields, flags));

	datalink_id_t dlid;
	int fd;
	ofmt_handle_t ofmt = NULL;
	ofmt_status_t oferr;

	oferr = ofmt_open(ofields, net_fields, flags, 0, &ofmt);
	if (oferr != OFMT_SUCCESS) {
		char ebuf[OFMT_BUFSIZE];

		errx(EXIT_FAILURE, "%s: ofmt_open failed: %s", __func__,
		    ofmt_strerror(ofmt, oferr, ebuf, sizeof (ebuf)));
	}

	get_linkid(ovname, &dlid);
	fd = open_overlay(overlay_dev, B_FALSE);

	for (int i = optind; i < argc; i++) {
		overlay_ioc_net_t net = {
			.oin_hdr.orih_linkid = dlid
		};
		int ret;

		(void) strlcpy(net.oin_id, argv[i], sizeof (net.oin_id));
		ret = ioctl(fd, OVERLAY_ROUTER_NET_GET, &net);
		if (ret != 0) {
			err(EXIT_FAILURE, "Failed to get info on router id %s",
			    argv[i]);
		}

		ofmt_print(ofmt, &net);
	}

	ofmt_close(ofmt);
	return (0);
}

static int
do_route_table(int argc, char **argv)
{
	return (dispatch(argc, argv, route_tbl_tbl, ARRAY_SIZE(route_tbl_tbl)));
}

static int
do_routetbl_create(int argc, char **argv)
{
	const char *ovname = NULL;
	overlay_ioc_routetab_t rtbl = { 0 };
	int c, ret;

	while ((c = getopt(argc, argv, "d:")) != -1) {
		switch (c) {
		case 'd':
			ovname = optarg;
			break;
		}
	}

	if (argc == optind) {
		(void) fprintf(stderr, "Missing router table id\n");
		usage();
	}

	if (ovname == NULL) {
		(void) fprintf(stderr, "Missing overlay device name\n");
		usage();
	}

	parse_id(argv[optind], rtbl.oir_id, sizeof (rtbl.oir_id));
	get_linkid(ovname, &rtbl.oir_hdr.orih_linkid);

	ret = do_ioctl(OVERLAY_ROUTETBL_SET, &rtbl, B_FALSE);
	if (ret != 0) {
		err(EXIT_FAILURE,
		    "failed to create overlay routing table %s on overlay %s",
		    argv[optind], ovname);
	}

	return (0);
}

static int
do_routetbl_delete(int argc, char **argv)
{
	const char *ovname = NULL;
	overlay_ioc_routetab_t rtbl = { 0 };
	int c, ret;

	while ((c = getopt(argc, argv, "d:")) != -1) {
		switch (c) {
		case 'd':
			ovname = optarg;
			break;
		}
	}

	if (argc == optind) {
		(void) fprintf(stderr, "Missing router table id\n");
		usage();
	}

	if (ovname == NULL) {
		(void) fprintf(stderr, "Missing overlay device name\n");
		usage();
	}

	parse_id(argv[optind], rtbl.oir_id, sizeof (rtbl.oir_id));
	get_linkid(ovname, &rtbl.oir_hdr.orih_linkid);

	ret = do_ioctl(OVERLAY_ROUTETBL_REMOVE, &rtbl, B_FALSE);
	if (ret != 0) {
		err(EXIT_FAILURE,
		    "failed to create overlay routing table %s on overlay %s",
		    argv[optind], ovname);
	}

	return (0);
}

static int
do_routetbl_set_default(int argc, char **argv)
{
	const char *ovname = NULL;
	int c, ret;
	overlay_ioc_routetab_t rtbl = { 0 };

	while ((c = getopt(argc, argv, "d:")) != -1) {
		switch (c) {
		case 'd':
			ovname = optarg;
			break;
		}
	}

	if (ovname == NULL) {
		(void) fprintf(stderr, "Overlay device name missing\n");
		usage();
	}

	if (argc < optind) {
		(void) fprintf(stderr, "Missing router table name\n");
		usage();
	}

	parse_id(argv[optind], rtbl.oir_id, sizeof (rtbl.oir_id));
	get_linkid(ovname, &rtbl.oir_hdr.orih_linkid);

	ret = do_ioctl(OVERLAY_ROUTETBL_SET_DEFAULT, &rtbl, B_FALSE);
	if (ret != 0) {
		err(EXIT_FAILURE,
		    "failed to set default routing table on %s to %s\n",
		    ovname, argv[optind]);
	}

	return (0);
}

static boolean_t
routetbl_print(ofmt_arg_t *ofmt, char *buf, uint_t buflen)
{
	overlay_ioc_routetab_t *tbl = ofmt->ofmt_cbarg;

	switch ((routetbl_member_t)ofmt->ofmt_id) {
	case RM_ID:
		(void) strlcpy(buf, tbl->oir_id, buflen);
		break;
	case RM_NENTS:
		(void) snprintf(buf, buflen, "%" PRIu16, tbl->oir_count);
		break;
	}

	return (B_TRUE);
}

static boolean_t
routeent_print(ofmt_arg_t *ofmt, char *buf, uint_t buflen)
{
	const routetbl_print_t *prt = ofmt->ofmt_cbarg;
	const overlay_route_ent_t *ent = prt->rpt_ent;
	const void *addr;
	in_addr_t v4;
	int af;
	uint8_t pfxlen = ent->ore_prefixlen;

	switch ((routetbl_ent_memb_t)ofmt->ofmt_id) {
	case REM_ID:
		(void) strlcpy(buf, prt->rpt_id, buflen);
		break;
	case REM_DEST:
		if (IN6_IS_ADDR_V4MAPPED(&ent->ore_dest)) {
			af = AF_INET;
			IN6_V4MAPPED_TO_IPADDR(&ent->ore_dest, v4);
			addr = &v4;
			pfxlen -= 96;
		} else {
			af = AF_INET6;
			addr = &ent->ore_dest;
		}

		netprefix_print(af, addr, pfxlen, buf, buflen);
		break;
	case REM_TARGET:
		(void) sockaddr_print(&ent->ore_target, buf, buflen);
		break;
	}

	return (B_TRUE);
}

static size_t
route_tbl_size(size_t nents)
{
	size_t hdr = sizeof (overlay_ioc_routetab_t);
	size_t entsz = nents * sizeof (overlay_route_ent_t);
	size_t tot = hdr + entsz;

	if (nents > 0 && entsz < nents)
		goto overflow;
	if (tot < hdr)
		goto overflow;

	return (tot);

overflow:
	(void) fprintf(stderr, "%s: %s: overflow nents=%zu entsz=%zu tot=%zu\n",
	    __progname, __func__, nents, entsz, tot);
	abort();
}

static overlay_ioc_routetab_t *
route_tbl_alloc(const char *ovname, size_t nents)
{
	overlay_ioc_routetab_t *tbl;

	tbl = umem_zalloc(route_tbl_size(nents), UMEM_NOFAIL);
	tbl->oir_count = nents;
	get_linkid(ovname, &tbl->oir_hdr.orih_linkid);

	return (tbl);
}

static void
route_tbl_free(overlay_ioc_routetab_t *tbl, size_t nents)
{
	if (tbl == NULL)
		return;

	umem_free(tbl, route_tbl_size(nents));
}

static overlay_ioc_rtab_iter_t *
route_tbl_iter_alloc(const char *ovname, size_t nents)
{
	overlay_ioc_rtab_iter_t *iter;
	size_t hdr = sizeof (overlay_ioc_rtab_iter_t);
	size_t entsz = nents * sizeof (overlay_ioc_routetab_t);
	size_t totsz = hdr + entsz;

	VERIFY3U(nents, <=, OVERLAY_ROUTER_ITER_MAX);

	if (nents > 0 && entsz < nents)
		goto overflow;
	if (totsz < hdr)
		goto overflow;

	iter = umem_zalloc(totsz, UMEM_NOFAIL);
	iter->oiri_count = nents;
	get_linkid(ovname, &iter->oiri_hdr.orih_linkid);

	return (iter);

overflow:
	(void) fprintf(stderr,
	    "%s: %s: overflow nents=%zu entsz=%zu totsz=%zu\n", __progname,
	    __func__, nents, entsz, totsz);
	abort();
}

static void
route_tbl_iter_free(overlay_ioc_rtab_iter_t *iter, size_t nents)
{
	if (iter == NULL)
		return;

	size_t len = sizeof (overlay_ioc_rtab_iter_t) +
	    nents * sizeof (overlay_ioc_routetab_t);

	umem_free(iter, len);
}

static int
do_routetbl_iter(const char *ovname, const char *ofields, uint_t flags)
{
	const size_t nents = 64;
	overlay_ioc_rtab_iter_t *iter;
	ofmt_handle_t ofmt;
	ofmt_status_t oferr;
	int fd;

	iter = route_tbl_iter_alloc(ovname, nents);
	fd = open_overlay(overlay_dev, B_TRUE);

	oferr = ofmt_open(ofields, routetbl_fields, flags, 0, &ofmt);
	if (oferr != OFMT_SUCCESS) {
		char ebuf[OFMT_BUFSIZE];

		errx(EXIT_FAILURE, "%s: ofmt_open failed: %s", __func__,
		    ofmt_strerror(ofmt, oferr, ebuf, sizeof (ebuf)));
	}

	for (;;) {
		int ret;

		ret = ioctl(fd, OVERLAY_ROUTETBL_ITER, iter);
		if (ret != 0)
			err(EXIT_FAILURE, "error iterating routing tables");

		if (iter->oiri_count == 0)
			break;

		for (uint_t i = 0; i < iter->oiri_count; i++)
			ofmt_print(ofmt, &iter->oiri_rtabs[i]);

		bzero(iter->oiri_rtabs,
		    nents * sizeof (overlay_ioc_routetab_t));
		iter->oiri_count = nents;
	}

	ofmt_close(ofmt);
	VERIFY0(close(fd));
	route_tbl_iter_free(iter, nents);
	return (0);
}

static int
do_routetbl_get(int argc, char **argv)
{
	const char *ovname = NULL;
	const char *ofields = "all";
	uint_t flags = 0;
	int c;

	while ((c = getopt(argc, argv, "d:o:p")) != -1) {
		switch (c) {
		case 'd':
			ovname = optarg;
			break;
		case 'o':
			ofields = optarg;
			break;
		case 'p':
			flags |= OFMT_PARSABLE;
			break;
		}
	}

	if (ovname == NULL) {
		(void) fprintf(stderr, "Missing overlay device name\n");
		usage();
	}

	if (optind == argc)
		return (do_routetbl_iter(ovname, ofields, flags));

	int fd;
	ofmt_handle_t ofmt = NULL;
	ofmt_status_t oferr;
	overlay_ioc_routetab_t *tbl = NULL;
	size_t nents = 64;

	oferr = ofmt_open(ofields, routeent_fields, flags, 0, &ofmt);
	if (oferr != OFMT_SUCCESS) {
		char ebuf[OFMT_BUFSIZE];

		errx(EXIT_FAILURE, "%s: ofmt_open failed: %s", __func__,
		    ofmt_strerror(ofmt, oferr, ebuf, sizeof (ebuf)));
	}

	fd = open_overlay(overlay_dev, B_FALSE);

	tbl = route_tbl_alloc(ovname, nents);

	for (int i = optind; i < argc; i++) {
		routetbl_print_t prt = { 0 };
		datalink_id_t linkid = tbl->oir_hdr.orih_linkid;
		int ret;

		(void) strlcpy(prt.rpt_id, argv[i], sizeof (prt.rpt_id));

		/*
		 * Clear out tbl to be used for the next route table.
		 * We need to re-populate the linkid and route table id
		 * though.
		 */
		bzero(tbl, sizeof (*tbl));
		tbl->oir_hdr.orih_linkid = linkid;
		(void) strlcpy(tbl->oir_id, argv[i], sizeof (tbl->oir_id));

		for (;;) {
			tbl->oir_count = nents;
			ret = ioctl(fd, OVERLAY_ROUTETBL_GET, tbl);
			if (ret != 0) {
				err(EXIT_FAILURE,
				    "failed to get routing table %s in %s",
				    argv[i], ovname);
			}

			if (tbl->oir_count == 0)
				break;

			for (size_t j = 0; j < tbl->oir_count; j++) {
				prt.rpt_ent = &tbl->oir_ents[j];
				ofmt_print(ofmt, &prt);
			}
		}
	}

	route_tbl_free(tbl, nents);
	ofmt_close(ofmt);
	VERIFY0(close(fd));
	return (0);
}

static int
do_routetbl_ent_common(int argc, char **argv, boolean_t add)
{
	const int cmd = add ? OVERLAY_ROUTETBL_ADDENT : OVERLAY_ROUTETBL_DELENT;
	const char *opstr = add ? "add" : "remove";

	const char *ovname = NULL;
	const char *rtbl_id = NULL;
	int c, ret;
	overlay_ioc_routetab_t *tbl = NULL;
	overlay_route_ent_t *ent = NULL;

	while ((c = getopt(argc, argv, "d:i:")) != -1) {
		switch (c) {
		case 'd':
			ovname = optarg;
			break;
		case 'i':
			rtbl_id = optarg;
			break;
		}
	}

	if (argc - optind < 2) {
		(void) fprintf(stderr, "missing destination and/or target\n");
		usage();
	}

	if (ovname == NULL) {
		(void) fprintf(stderr, "missing overlay device name\n");
		usage();
	}

	if (rtbl_id == NULL) {
		(void) fprintf(stderr, "Missing routing table id\n");
		usage();
	}

	tbl = route_tbl_alloc(ovname, 1);
	ent = tbl->oir_ents;

	parse_id(rtbl_id, tbl->oir_id, sizeof (tbl->oir_id));
	get_linkid(ovname, &tbl->oir_hdr.orih_linkid);

	parse_addr(argv[optind], &ent->ore_dest, &ent->ore_prefixlen);
	parse_addr_port(argv[optind + 1], &ent->ore_target);

	ret = do_ioctl(cmd, tbl, B_FALSE);
	if (ret != 0) {
		err(EXIT_FAILURE, "failed to %s entry", opstr);
	}

	route_tbl_free(tbl, 1);
	return (0);
}

static int
do_routetbl_addent(int argc, char **argv)
{
	return (do_routetbl_ent_common(argc, argv, B_TRUE));
}

static int
do_routetbl_delent(int argc, char **argv)
{
	return (do_routetbl_ent_common(argc, argv, B_FALSE));
}

static int
do_ioctl(int cmd, void *arg, boolean_t ro)
{
	int fd = open_overlay(overlay_dev, ro);
	int ret = ioctl(fd, cmd, arg);
	int errsave = errno;
	VERIFY0(close(fd));
	errno = errsave;
	return (ret);
}

static void
get_linkid(const char *name, datalink_id_t *linkp)
{
	dladm_handle_t handle = NULL;
	dladm_status_t status;
	char errmsg[DLADM_STRSIZE] = { 0 };

	status = dladm_open(&handle);
	if (status != DLADM_STATUS_OK) {
		errx(EXIT_FAILURE, "could not open /dev/dld: %s",
		    dladm_status2str(status, errmsg));
	}

	status = dladm_name2info(handle, name, linkp, NULL, NULL, NULL);
	if (status != DLADM_STATUS_OK) {
		errx(EXIT_FAILURE, "failed to find %s: %s", name,
		    dladm_status2str(status, errmsg));
	}

	dladm_close(handle);
}

static int
open_overlay(const char *ovname, boolean_t ro)
{
	int fd;

	fd = open(overlay_dev, ro ? O_RDONLY : O_RDWR);
	if (fd == -1)
		err(EXIT_FAILURE, "failed to open %s", overlay_dev);

	return (fd);
}

static void
parse_vlan(const char *str, uint16_t *valp)
{
	unsigned long uval;

	errno = 0;
	uval = strtoul(str, NULL, 0);
	if (errno != 0)
		err(EXIT_FAILURE, "Failed to parse '%s' as a vlan id", str);

	if (uval < VLAN_ID_MIN || uval > VLAN_ID_MAX) {
		errx(EXIT_FAILURE, "Vlan id %lu out of range (%d-%d)\n", uval,
		    VLAN_ID_MIN, VLAN_ID_MAX);
	}

	*valp = uval;
}

static void
parse_mac(const char *str, uint8_t *macp)
{
	if (ether_aton_r(str, (struct ether_addr *)macp) == NULL)
		errx(EXIT_FAILURE, "Invalid mac address '%s'", str);
}

static void
parse_id(const char *str, char *id, size_t idlen)
{
	static regex_t re = { 0 };
	static boolean_t re_compiled = B_FALSE;
	int ret;

	if (!re_compiled) {
		VERIFY0(regcomp(&re, id_re, REG_EXTENDED));
		re_compiled = B_TRUE;
	}

	ret = regexec(&re, str, 0, NULL, 0);
	if (ret != 0)
		errx(EXIT_FAILURE, "Invalid id '%s'", str);

	/*
	 * The regex also validates the length of the id, so truncation
	 * here should not be a concern (but stlcpy() is still used as a
	 * precaution).
	 */
	(void) strlcpy(id, str, idlen);
}

/*
 * To parse a lone address, prefixlenp should be NULL. If prefixlenp is
 * non-NULL, attempt to parse '/nnn' after the address as the
 * prefix length (if missing from the string, assume /128 -- i.e. single
 * address).
 */
static void
parse_addr(const char *str, struct in6_addr *addrp, uint8_t *prefixlenp)
{
	char *astr = strdup(str);
	char *pfxstr = astr;
	int af;

	if (astr == NULL) {
		(void) fprintf(stderr, "Out of memory\n");
		abort();
	}

	/*
	 * This should break things up into two strings -- the address (astr)
	 * and the prefix (pfxstr). If no prefix present, pfxstr should be
	 * NULL.
	 */
	(void) strsep(&pfxstr, "/");

	/*
	 * If the string contains a '.', we assume it's IPv4, otherwise
	 * assume IPv6. XXX: is there a better way here?
	 */
	if (strchr(astr, '.') != NULL) {
		in_addr_t v4;

		if (inet_pton(AF_INET, astr, &v4) != 1)
			err(EXIT_FAILURE, "Invalid IPv4 address '%s'", astr);

		IN6_IPADDR_TO_V4MAPPED(v4, addrp);
		af = AF_INET;
	} else {
		if (inet_pton(AF_INET6, astr, addrp) != 1)
			err(EXIT_FAILURE, "Invalid IPv6 address '%s'", astr);
		af = AF_INET6;
	}

	if (prefixlenp == NULL) {
		if (pfxstr != NULL)
			errx(EXIT_FAILURE, "Invalid address '%s'", str);

		free(astr);
		return;
	}

	if (pfxstr == NULL) {
		*prefixlenp = 128;
		free(astr);
		return;
	}

	unsigned long pval;
	const unsigned long max = (af == AF_INET) ? 32 : 128;

	errno = 0;
	pval = strtoul(pfxstr, NULL, 10);
	if (errno != 0) {
		err(EXIT_FAILURE, "Failed to parse '%s' as a prefix length",
		    pfxstr);
	}

	if (pval > max) {
		err(EXIT_FAILURE, "Invalid prefix length '%lu'", pval);
	}

	*prefixlenp = (af == AF_INET) ? 128 - (32 - pval) : pval;
	free(astr);
}

/*
 * Parse address + port. Supported strings:
 *	<IPv4 Addr>
 *	<IPv4 Addr>:port
 *	<IPv6 Addr>
 *	[<IPv4 or IPv6 addr>]:port
 *
 * The latter is required for IPv6 (but optional for IPv4) to disambiguate
 * between the IPv6 address and port.
 */
static void
parse_addr_port(const char *str, struct sockaddr_in6 *addrp)
{
	char *astr = strdup(str);
	char *portstr = astr;
	unsigned long pval;

	if (astr == NULL) {
		(void) fprintf(stderr, "Out of memory\n");
		abort();
	}

	bzero(addrp, sizeof (struct sockaddr_in6));

	/*
	 * Since IPv4 addresses are mapped to IPv6, we always set this
	 * to AF_INET6 for consistency.
	 */
	addrp->sin6_family = AF_INET6;

	if (str[0] != '[') {
		if (strchr(astr, '.') != NULL) {
			in_addr_t v4;

			(void) strsep(&portstr, ":");

			if (inet_pton(AF_INET, astr, &v4) != 1) {
				errx(EXIT_FAILURE, "Invalid IPv4 address '%s'",
				    astr);
			}

			IN6_IPADDR_TO_V4MAPPED(v4, &addrp->sin6_addr);
		} else {
			/*
			 * If it's an unbracketed IPv6 address, there is no
			 * port, only zuu^Wan address.
			 */
			portstr = NULL;

			if (inet_pton(AF_INET6, astr, &addrp->sin6_addr) != 1) {
				errx(EXIT_FAILURE, "Invalid IPv6 address '%s'",
				    astr);
			}
		}
	} else {
		char *p = astr + 1;

		/* Find the matching ']' */
		(void) strsep(&portstr, "]");
		if (portstr == NULL)
			errx(EXIT_FAILURE, "Unmatched ']' in address");

		/*
		 * If there's a port, the next character should be ':'
		 * e.g. '[1:2::3]:5678'. If not, *portstr should point
		 * to the original terminating NUL.
		 */
		if (*portstr == ':') {
			portstr++;
		} else {
			if (*portstr != '\0') {
				errx(EXIT_FAILURE, "Invalid address/port '%s'",
				    str);
			}
			portstr = NULL;
		}

		if (strchr(p, '.') != NULL) {
			in_addr_t v4;

			if (inet_pton(AF_INET, p, &v4) != 1) {
				err(EXIT_FAILURE, "Invalid IPv4 address '%s'",
				    p);
			}

			IN6_IPADDR_TO_V4MAPPED(v4, &addrp->sin6_addr);
		} else {
			if (inet_pton(AF_INET6, p, &addrp->sin6_addr) != 1) {
				err(EXIT_FAILURE, "Invalid IPv6 address '%s'",
				    p);
			}
		}
	}

	/* If no port given, just leave it as default (0) */
	if (portstr == NULL) {
		free(astr);
		return;
	}

	errno = 0;
	pval = strtoul(portstr, NULL, 10);
	if (errno != 0) {
		err(EXIT_FAILURE, "Failed to parse port value '%s'",
		    portstr);
	}

	if (pval > UINT16_MAX) {
		errx(EXIT_FAILURE, "Port value '%lu' out of range",
		    pval);
	}

	addrp->sin6_port = htons(pval);
	free(astr);
}

/*
 * Debug builds are automatically wired up for umem debugging.
 */
#ifdef  DEBUG
const char *
_umem_debug_init()
{
	return ("default,verbose");
}

const char *
_umem_logging_init(void)
{
	return ("fail,contents");
}
#endif  /* DEBUG */
