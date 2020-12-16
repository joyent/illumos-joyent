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

/*
 * This handles the layer 3 routing aspect of an overlay device.
 */

#include <sys/types.h>
#include <sys/containerof.h>
#include <sys/ethernet.h>
#include <sys/vlan.h>
#include <sys/file.h>
#include <sys/sysmacros.h>
#include <sys/policy.h>
#include <sys/overlay_impl.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/strsun.h>
#include <sys/ctype.h>

#include <netinet/arp.h>
#include <netinet/ip6.h>
#include <netinet/icmp6.h>

typedef enum overlay_router_ioctl_flags {
	OFF_NONE =	0,		/* no flags */
	OFF_RW =	(1 << 0),	/* RW access required */
	OFF_COPYOUT =	(1 << 2)	/* Perform copyout on success */
} overlay_router_ioctl_flags_t;

typedef int (*overlay_router_copyin_f)(const void *, void **, size_t *, int);
typedef int (*overlay_router_ioctl_f)(overlay_router_t *, void *);

typedef struct overlay_router_ioctl {
	int				ori_cmd;
	overlay_router_ioctl_flags_t	ori_flags;
	overlay_router_copyin_f		ori_copyin;
	overlay_router_ioctl_f		ori_func;
	size_t				ori_size;
} overlay_router_ioctl_t;

/* These exist in the kernel, but don't seem to have header files for them */
extern void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
extern void *bsearch(const void *, const void *, size_t, size_t,
    int (*)(const void *, const void *));

extern uint8_t overlay_macaddr[ETHERADDRL];

static kmem_cache_t *overlay_router_cache;
static kmem_cache_t *overlay_net_cache;
static kmem_cache_t *overlay_rtab_cache;

static int overlay_net_cache_ctor(void *, void *, int);

static inline void
net_addr(in_addr_t *dest, const in_addr_t *src, uint8_t prefixlen)
{
	const in_addr_t mask = htonl(((in_addr_t)1 << (32 - prefixlen)) - 1);
	*dest = *src & ~mask;
}

static inline boolean_t
net_addr_prefixequal(const in_addr_t *a1, const in_addr_t *a2,
    uint8_t prefixlen)
{
	in_addr_t cmp1, cmp2;

	net_addr(&cmp1, a1, prefixlen);
	net_addr(&cmp2, a2, prefixlen);
	return ((cmp1 == cmp2) ? B_TRUE : B_FALSE);
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

/*
 * A wrapper around the macro for both better error checking, and for
 * consistency with net_addr_prefixequal.
 */
static inline boolean_t
net_addr6_prefixequal(const struct in6_addr *a1, const struct in6_addr *a2,
    uint8_t prefixlen)
{
	return (IN6_ARE_PREFIXEDADDR_EQUAL(a1, a2, prefixlen) ?
	    B_TRUE : B_FALSE);
}

static boolean_t
overlay_valid_id(const char *str, size_t len)
{
	/*
	 * IDs are in fixed sized buffers. It should fit in the given length
	 * and be NUL-terminated.
	 */
	if (strnlen(str, len) == len && str[len - 1] != '\0')
		return (B_FALSE);

	/* Require the first character to be alphanumeric */
	if (!ISALNUM(*str))
		return (B_FALSE);
	str++;

	/* The remaining characters can be alphanumeric or include '-' or '.' */
	while (*str != '\0') {
		if (!ISALNUM(*str) && *str != '-' && *str != '.' && *str != '#')
			return (B_FALSE);
		str++;
	}
	return (B_TRUE);
}

static int
overlay_route_ent_cmp(const void *a, const void *b)
{
	const overlay_route_ent_t *l = a;
	const overlay_route_ent_t *r = b;
	int ret;

	/*
	 * Sort in reverse order -- 'largest' to 'smallest' (at least when
	 * treating the destination as a 128-bit integer). This should put
	 * more specific matches first when sorting.
	 */
	ret = memcmp(&l->ore_dest, &r->ore_dest, sizeof (l->ore_dest));
	if (ret < 0)
		return (1);
	if (ret > 0)
		return (-1);

	/*
	 * If we have a tie, look at the prefix length and sort in order
	 * of longest prefix to shortest.
	 */
	if (l->ore_prefixlen < r->ore_prefixlen)
		return (1);
	if (l->ore_prefixlen > r->ore_prefixlen)
		return (-1);

	/*
	 * Finally sort on target address. While we really don't care about
	 * ordering from largest to smallest here (when there are multiple
	 * targets for the longest prefix match, we'll typically hash on all
	 * of the targets), we define an order similar to above just to
	 * have one -- that way we can still easily locate _exact_ entries
	 * for removal.
	 */
	ret = memcmp(&l->ore_target, &r->ore_target, sizeof (l->ore_target));
	if (ret < 0)
		return (1);
	if (ret > 0)
		return (-1);
	return (0);
}

static overlay_routetab_t *
overlay_route_tbl_hold_by_id(overlay_router_t *orr, const char *id)
{
	overlay_routetab_t *rtab;
	list_t *rlist = &orr->orr_routetbls;

	mutex_enter(&orr->orr_lock);
	for (rtab = list_head(rlist); rtab != NULL;
	    rtab = list_next(rlist, rtab)) {
		if (strcmp(rtab->ort_id, id) == 0) {
			mutex_enter(&rtab->ort_lock);
			rtab->ort_refcnt++;
			mutex_exit(&rtab->ort_lock);
			mutex_exit(&orr->orr_lock);
			return (rtab);
		}
	}
	mutex_exit(&orr->orr_lock);
	return (NULL);
}

static void
overlay_route_tbl_rele(overlay_routetab_t *rtab)
{
	uint_t cnt;

	mutex_enter(&rtab->ort_lock);
	ASSERT3U(rtab->ort_refcnt, >, 0);
	cnt = --rtab->ort_refcnt;
	mutex_exit(&rtab->ort_lock);

	if (cnt != 0)
		return;

	VERIFY(!list_link_active(&rtab->ort_link));

	if (rtab->ort_routev4 != NULL) {
		kmem_free(rtab->ort_routev4,
		    rtab->ort_routev4_alloc * sizeof (overlay_route_ent_t));
		rtab->ort_routev4 = NULL;
	}

	if (rtab->ort_routev6 != NULL) {
		kmem_free(rtab->ort_routev6,
		    rtab->ort_routev6_alloc * sizeof (overlay_route_ent_t));
		rtab->ort_routev6 = NULL;
	}

	kmem_cache_free(overlay_rtab_cache, rtab);
}

static overlay_net_t *
overlay_hold_net_by_id(overlay_router_t *orr, const char *id)
{
	overlay_net_t *ont;

	mutex_enter(&orr->orr_lock);
	/*
	 * Since every net must have a valid MAC address, we just
	 * search the mac AVL tree. We can always do something different
	 * in the future if this turns out to be too slow.
	 */
	for (ont = avl_first(&orr->orr_nets_mac); ont != NULL;
	    ont = AVL_NEXT(&orr->orr_nets_mac, ont)) {
		if (strcmp(ont->ont_id, id) == 0) {
			mutex_enter(&ont->ont_lock);
			ont->ont_refcnt++;
			mutex_exit(&ont->ont_lock);
			mutex_exit(&orr->orr_lock);
			return (ont);
		}
	}
	mutex_exit(&orr->orr_lock);
	return (NULL);
}

/* vlan is in host byte order */
overlay_net_t *
overlay_hold_net_by_vlan(overlay_router_t *orr, uint16_t vlan)
{
	overlay_net_t *ont = NULL;
	overlay_net_t ref = {
		.ont_vlan = vlan
	};

	mutex_enter(&orr->orr_lock);
	ont = avl_find(&orr->orr_nets_vlan, &ref, NULL);
	if (ont != NULL) {
		mutex_enter(&ont->ont_lock);
		ont->ont_refcnt++;
		mutex_exit(&ont->ont_lock);
	}
	mutex_exit(&orr->orr_lock);
	return (ont);
}

overlay_net_t *
overlay_hold_net_by_mac(overlay_router_t *orr, const uint8_t mac[ETHERADDRL])
{
	overlay_net_t *ont = NULL;
	overlay_net_t ref = { 0 };

	bcopy(mac, ref.ont_mac, ETHERADDRL);

	mutex_enter(&orr->orr_lock);
	ont = avl_find(&orr->orr_nets_mac, &ref, NULL);
	if (ont != NULL) {
		mutex_enter(&ont->ont_lock);
		ont->ont_refcnt++;
		mutex_exit(&ont->ont_lock);
	}
	mutex_exit(&orr->orr_lock);
	return (ont);
}

/*
 * Find and hold the overlay_net_t whose subnet contains addr, and return the
 * overlay_net_t, or NULL if not found.
 */
overlay_net_t *
overlay_hold_net_by_ip(overlay_router_t *orr, in_addr_t addr)
{
	overlay_net_t *ont;
	avl_index_t where;
	overlay_net_t ref = {
		.ont_net = addr
	};

	mutex_enter(&orr->orr_lock);

	/*
	 * The nets_v4 avl tree orders the entries by the network address,
	 * e.g. 10.0.1.0, 172.16.5.0, 192.168.1.0, 200.200.0.0, ...
	 *
	 * We use the fact that the network address is always the first
	 * (and thus lowest numerically) address in a subnet. When we search
	 * for the address, it should normally fail (unless it's an actual
	 * network address we have), however if the network exists, the
	 * AVL_BEFORE entry should be the network address that contains
	 * 'addr'. We verify and return, otherwise the network isn't present.
	 */
	ont = avl_find(&orr->orr_nets_v4, &ref, &where);
	if (ont != NULL)
		goto out;

	ont = avl_nearest(&orr->orr_nets_v4, where, AVL_BEFORE);
	if (ont == NULL) {
		mutex_exit(&orr->orr_lock);
		return (NULL);
	}

	net_addr(&addr, &addr, ont->ont_prefixlen);
	if (addr != ont->ont_net) {
		mutex_exit(&orr->orr_lock);
		return (NULL);
	}

out:
	mutex_enter(&ont->ont_lock);
	ont->ont_refcnt++;
	mutex_exit(&ont->ont_lock);
	mutex_exit(&orr->orr_lock);
	return (ont);
}

/* Similar to overlay_net_hold_by_net() except for an IPv6 addr. */
overlay_net_t *
overlay_hold_net_by_ip6(overlay_router_t *orr, const struct in6_addr *addr)
{
	overlay_net_t *ont;
	avl_index_t where;
	overlay_net_t ref = { 0 };

	bcopy(addr, &ref.ont_netv6, sizeof (ref.ont_netv6));

	mutex_enter(&orr->orr_lock);

	/*
	 * We use a similar strategy as in overlay_net_hold_by_net() to
	 * locate the net that contains 'addr'.
	 */
	ont = avl_find(&orr->orr_nets_v6, &ref, &where);
	if (ont != NULL)
		goto out;

	ont = avl_nearest(&orr->orr_nets_v6, where, AVL_BEFORE);
	if (ont == NULL) {
		mutex_exit(&orr->orr_lock);
		return (NULL);
	}

	if (!net_addr6_prefixequal(addr, &ont->ont_netv6,
	    ont->ont_prefixlenv6)) {
		mutex_exit(&orr->orr_lock);
		return (NULL);
	}

out:
	mutex_enter(&ont->ont_lock);
	ont->ont_refcnt++;
	mutex_exit(&ont->ont_lock);
	mutex_exit(&orr->orr_lock);
	return (ont);
}

void
overlay_net_rele(overlay_net_t *ont)
{
	uint_t cnt;

	mutex_enter(&ont->ont_lock);
	ASSERT3U(ont->ont_refcnt, >, 0);
	cnt = --ont->ont_refcnt;
	mutex_exit(&ont->ont_lock);

	if (cnt != 0)
		return;

	if (ont->ont_routetbl != NULL) {
		overlay_route_tbl_rele(ont->ont_routetbl);
		ont->ont_routetbl = NULL;
	}

	/*
	 * It's simpler to just destroy/re-init the mutex so that what
	 * we return to the kmem cache looks exactly like an initialized
	 * overlay_net_t
	 */
	mutex_destroy(&ont->ont_lock);
	(void) overlay_net_cache_ctor(ont, NULL, 0);

	kmem_cache_free(overlay_net_cache, ont);
}

static int
overlay_router_hold_by_dlid(datalink_id_t id, overlay_router_t **orrp)
{
	overlay_dev_t *odd;
	overlay_router_t *orr;

	odd = overlay_hold_by_dlid(id);
	if (odd == NULL)
		return (ENOENT);

	mutex_enter(&odd->odd_lock);
	if (!(odd->odd_flags & OVERLAY_F_VARPD)) {
		mutex_exit(&odd->odd_lock);
		overlay_hold_rele(odd);
		return (ENXIO);
	}
	orr = odd->odd_router;

	mutex_enter(&orr->orr_lock);
	mutex_exit(&odd->odd_lock);

	orr->orr_refcnt++;
	mutex_exit(&orr->orr_lock);

	*orrp = orr;
	return (0);
}

static void
overlay_router_rele(overlay_router_t *orr)
{
	mutex_enter(&orr->orr_lock);
	orr->orr_refcnt--;
	mutex_exit(&orr->orr_lock);
}

overlay_router_t *
overlay_router_create(overlay_dev_t *odd __unused)
{
	overlay_router_t *orr;

	orr = kmem_cache_alloc(overlay_router_cache, KM_SLEEP);
	return (orr);
}

void
overlay_router_free(overlay_router_t *orr)
{
	if (orr == NULL)
		return;

	kmem_cache_free(overlay_router_cache, orr);
}

boolean_t
overlay_router_active(overlay_router_t *orr)
{
	ASSERT(MUTEX_HELD(&orr->orr_lock));
	return ((avl_numnodes(&orr->orr_nets_mac) > 0) ? B_TRUE : B_FALSE);
}

static int
overlay_router_net_create(overlay_router_t *orr, void *buf)
{
	overlay_ioc_net_t *ioc_net = buf;
	overlay_net_t *net = NULL;
	overlay_routetab_t *rtab = NULL;
	boolean_t hasv4 = B_FALSE;
	boolean_t hasv6 = B_FALSE;

	/*
	 * Validate the addresses -- we need at least one of:
	 * (IPv4 network address, IPv4 router address, IPv4 prefix length) or
	 * (IPv6 network address, IPv6 router addresss, IPv6 prefix length)
	 */
	if (ioc_net->oin_routeraddr != INADDR_ANY) {
		if (ioc_net->oin_prefixlen > 32)
			return (EINVAL);
		hasv4 = B_TRUE;
	}
	if (!IN6_IS_ADDR_UNSPECIFIED(&ioc_net->oin_routeraddrv6)) {
		if (ioc_net->oin_prefixlenv6 > 128)
			return (EINVAL);
		hasv6 = B_TRUE;
	}
	if (!hasv4 && !hasv6)
		return (EINVAL);

	/* Validate vlan id */
	if (ioc_net->oin_vlan < VLAN_ID_MIN || ioc_net->oin_vlan > VLAN_ID_MAX)
		return (EINVAL);

	/* MAC address can't be all zeros */
	if (bcmp(ioc_net->oin_mac, overlay_macaddr, ETHERADDRL) == 0)
		return (EINVAL);

	if (!overlay_valid_id(ioc_net->oin_id, sizeof (ioc_net->oin_id)))
		return (EINVAL);

	if (overlay_valid_id(ioc_net->oin_routetbl,
	    sizeof (ioc_net->oin_routetbl))) {
		rtab = overlay_route_tbl_hold_by_id(orr, ioc_net->oin_routetbl);
		if (rtab == NULL)
			return (EINVAL);
	} else if (ioc_net->oin_routetbl[0] != '\0') {
		/*
		 * The route table ID was not empty, but invalid. We
		 * allow an empty route table ID to indicate the default
		 * route table should be used (if it exists).
		 */
		return (EINVAL);
	}

	net = kmem_cache_alloc(overlay_net_cache, KM_SLEEP);

	/*
	 * Set the initial refhold. We use this so that we can
	 * just use overlay_net_rele() and have things work correctly if we
	 * fail at some point.
	 */
	net->ont_refcnt++;

	(void) strlcpy(net->ont_id, ioc_net->oin_id, sizeof (net->ont_id));
	(void) bcopy(ioc_net->oin_mac, net->ont_mac, ETHERADDRL);
	net->ont_routetbl = rtab;

	/*
	 * Since we use mac_vlan_header_info() for any routed packets, we
	 * have a copy of the VLAN id in host byte order. Keep it that way
	 * to make debugging simpler.
	 */
	net->ont_vlan = ioc_net->oin_vlan;

	net->ont_routeraddr = ioc_net->oin_routeraddr;
	net->ont_prefixlen = ioc_net->oin_prefixlen;
	net_addr(&net->ont_net, &net->ont_routeraddr, net->ont_prefixlen);

	bcopy(&ioc_net->oin_routeraddrv6, &net->ont_routeraddrv6,
	    sizeof (net->ont_routeraddrv6));
	net->ont_prefixlenv6 = ioc_net->oin_prefixlenv6;
	net_addr6(&net->ont_netv6, &net->ont_routeraddrv6,
	    net->ont_prefixlenv6);

	mutex_enter(&orr->orr_lock);
	mutex_enter(&net->ont_lock);

	if (avl_find(&orr->orr_nets_vlan, net, NULL) != NULL ||
	    avl_find(&orr->orr_nets_mac, net, NULL) != NULL ||
	    (hasv4 && avl_find(&orr->orr_nets_v4, net, NULL) != NULL) ||
	    (hasv6 && avl_find(&orr->orr_nets_v6, net, NULL) != NULL)) {
		mutex_exit(&net->ont_lock);
		mutex_exit(&orr->orr_lock);

		overlay_net_rele(net);
		return (EEXIST);
	}

	avl_add(&orr->orr_nets_vlan, net);
	net->ont_refcnt++;

	avl_add(&orr->orr_nets_mac, net);
	net->ont_refcnt++;

	if (hasv4) {
		avl_add(&orr->orr_nets_v4, net);
		net->ont_refcnt++;
	}

	if (hasv6) {
		avl_add(&orr->orr_nets_v6, net);
		net->ont_refcnt++;
	}

	mutex_exit(&net->ont_lock);
	mutex_exit(&orr->orr_lock);

	/* Release the initial hold we took immediately after allocation. */
	overlay_net_rele(net);
	return (0);
}

static int
overlay_router_net_delete(overlay_router_t *orr, void *buf)
{
	overlay_ioc_net_t *rnet = buf;
	overlay_net_t *net = NULL;

	/*
	 * Require all other fields aside from the overlay id and the
	 * router id to be zero. We want to prevent a scenario where
	 * current clients pass in garbage for currently unused fields
	 * (and have it work) -- if other fields are used in the future this
	 * could be a source of interesting bugs.
	 */
	if (!IN6_IS_ADDR_UNSPECIFIED(&rnet->oin_routeraddrv6) ||
	    rnet->oin_routeraddr != INADDR_ANY ||
	    rnet->oin_prefixlen != 0 ||
	    rnet->oin_prefixlenv6 != 0 ||
	    rnet->oin_vlan != 0 ||
	    bcmp(rnet->oin_mac, overlay_macaddr, ETHERADDRL) != 0 ||
	    strlen(rnet->oin_routetbl) != 0)
		return (EINVAL);

	if (!overlay_valid_id(rnet->oin_id, sizeof (rnet->oin_id)))
		return (EINVAL);

	if (strlen(rnet->oin_id) == 0)
		return (EINVAL);

	net = overlay_hold_net_by_id(orr, rnet->oin_id);
	if (net == NULL)
		return (ENOENT);

	mutex_enter(&orr->orr_lock);
	mutex_enter(&net->ont_lock);

	avl_remove(&orr->orr_nets_vlan, net);
	net->ont_refcnt--;

	avl_remove(&orr->orr_nets_mac, net);
	net->ont_refcnt--;

	if (net->ont_routeraddr != INADDR_ANY) {
		avl_remove(&orr->orr_nets_v4, net);
		net->ont_refcnt--;
	}

	if (!IN6_IS_ADDR_UNSPECIFIED(&net->ont_routeraddrv6)) {
		avl_remove(&orr->orr_nets_v6, net);
		net->ont_refcnt--;
	}
	mutex_exit(&orr->orr_lock);
	mutex_exit(&net->ont_lock);

	overlay_net_rele(net);
	return (0);
}

static int
overlay_router_net_delete_all(overlay_router_t *orr, void *buf)
{
	overlay_ioc_net_t *rnet = buf;

	/*
	 * Similar to overlay_router_delete(), we enforce that only
	 * the overlay id is set, and all other fields are 0.
	 */
	if (!IN6_IS_ADDR_UNSPECIFIED(&rnet->oin_routeraddrv6) ||
	    rnet->oin_routeraddr != INADDR_ANY ||
	    rnet->oin_prefixlen != 0 ||
	    rnet->oin_prefixlenv6 != 0 ||
	    rnet->oin_vlan != 0 ||
	    bcmp(rnet->oin_mac, overlay_macaddr, ETHERADDRL) != 0 ||
	    strlen(rnet->oin_id) != 0 ||
	    strlen(rnet->oin_routetbl) != 0)
		return (EINVAL);

	/* TODO */

	return (0);
}

static int
overlay_router_net_get(overlay_router_t *orr, void *buf)
{
	overlay_ioc_net_t *ioc_net = buf;
	overlay_net_t *net = NULL;

	/*
	 * Require all other fields aside from the overlay id and the
	 * router id to be zero. We want to prevent a scenario where
	 * current clients pass in garbage for currently unused fields
	 * (and have it work) -- if other fields are used in the future this
	 * could be a source of interesting bugs.
	 */
	if (!IN6_IS_ADDR_UNSPECIFIED(&ioc_net->oin_routeraddrv6) ||
	    ioc_net->oin_routeraddr != INADDR_ANY ||
	    ioc_net->oin_prefixlen != 0 ||
	    ioc_net->oin_prefixlenv6 != 0 ||
	    ioc_net->oin_vlan != 0 ||
	    bcmp(ioc_net->oin_mac, overlay_macaddr, ETHERADDRL) != 0 ||
	    strlen(ioc_net->oin_routetbl) != 0)
		return (EINVAL);

	if (!overlay_valid_id(ioc_net->oin_id, sizeof (ioc_net->oin_id)))
		return (EINVAL);

	if (strlen(ioc_net->oin_id) == 0)
		return (EINVAL);

	net = overlay_hold_net_by_id(orr, ioc_net->oin_id);
	if (net == NULL)
		return (ENOENT);

	mutex_enter(&orr->orr_lock);
	mutex_enter(&net->ont_lock);

	if (net->ont_routetbl != NULL) {
		(void) strlcpy(ioc_net->oin_routetbl, net->ont_routetbl->ort_id,
		    sizeof (ioc_net->oin_routetbl));
	}

	mutex_exit(&net->ont_lock);
	mutex_exit(&orr->orr_lock);

	bcopy(net->ont_mac, ioc_net->oin_mac, ETHERADDRL);
	ioc_net->oin_vlan = net->ont_vlan;

	ioc_net->oin_routeraddr = net->ont_routeraddr;
	ioc_net->oin_prefixlen = net->ont_prefixlen;

	bcopy(&net->ont_routeraddrv6, &ioc_net->oin_routeraddrv6,
	    sizeof (ioc_net->oin_routeraddrv6));
	ioc_net->oin_prefixlenv6 = net->ont_prefixlenv6;

	overlay_net_rele(net);
	return (0);
}

static int
overlay_router_iter_copyin(const void *ubuf, void **outp, size_t *bsize,
    int flags)
{
	overlay_ioc_net_iter_t base, *iter;

	if (ddi_copyin(ubuf, &base, sizeof (base), flags & FKIOCTL) != 0)
		return (EFAULT);

	if (base.oini_count == 0)
		return (EINVAL);

	*bsize = sizeof (base) +
	    base.oini_count * sizeof (overlay_ioc_net_t);
	iter = kmem_zalloc(*bsize, KM_SLEEP);
	bcopy(&base, iter, sizeof (base));

	*outp = iter;
	return (0);
}

typedef struct overlay_router_marker {
	uint8_t		orm_mac[ETHERADDRL];
	uint16_t	orm_done;
} overlay_router_marker_t;
CTASSERT(sizeof (overlay_router_marker_t) == sizeof (uint64_t));

static int
overlay_router_net_iter(overlay_router_t *orr, void *buf)
{
	overlay_ioc_net_iter_t *iter = buf;
	overlay_net_t lookup, *ent;
	overlay_router_marker_t *mark;
	avl_index_t where;
	avl_tree_t *avl;
	uint16_t written = 0;

	mark = (void *)&iter->oini_marker;

	if (mark->orm_done != 0) {
		iter->oini_count = 0;
		return (0);
	}

	avl = &orr->orr_nets_mac;

	mutex_enter(&orr->orr_lock);

	bcopy(mark->orm_mac, lookup.ont_mac, ETHERADDRL);
	ent = avl_find(avl, &lookup, &where);

	if (ent == NULL) {
		ent = avl_nearest(avl, where, AVL_AFTER);
		if (ent == NULL) {
			mark->orm_done = 1;
			goto done;
		}
	}

	for (; ent != NULL && written < iter->oini_count;
	    ent = AVL_NEXT(avl, ent)) {
		overlay_ioc_net_t *rnet = &iter->oini_ents[written++];

		bzero(rnet, sizeof (*rnet));

		mutex_enter(&ent->ont_lock);
		ent->ont_refcnt++;

		/*
		 * This is the only RW field in ent, since we take a ref,
		 * this is the only field that needs to be copied while
		 * holding ont_lock.
		 */
		if (ent->ont_routetbl != NULL) {
			(void) strlcpy(rnet->oin_routetbl,
			    ent->ont_routetbl->ort_id,
			    sizeof (rnet->oin_routetbl));
		}
		mutex_exit(&ent->ont_lock);

		bcopy(&ent->ont_routeraddrv6, &rnet->oin_routeraddrv6,
		    sizeof (rnet->oin_routeraddrv6));
		bcopy(ent->ont_mac, rnet->oin_mac, ETHERADDRL);
		(void) strlcpy(rnet->oin_id, ent->ont_id,
		    sizeof (rnet->oin_id));
		rnet->oin_routeraddr = ent->ont_routeraddr;
		rnet->oin_prefixlen = ent->ont_prefixlen;
		rnet->oin_prefixlenv6 = ent->ont_prefixlenv6;
		rnet->oin_vlan = ent->ont_vlan;

		bcopy(ent->ont_mac, mark->orm_mac, ETHERADDRL);

		mutex_enter(&ent->ont_lock);
		ent->ont_refcnt--;
		mutex_exit(&ent->ont_lock);
	}

	if (ent == NULL) {
		mark->orm_done = 1;
	}

done:
	iter->oini_count = written;
	mutex_exit(&orr->orr_lock);
	return (0);
}

static int
overlay_router_net_set_routetbl(overlay_router_t *orr, void *buf)
{
	overlay_ioc_net_t *rnet = buf;
	overlay_net_t *net;
	overlay_routetab_t *rtab = NULL;

	/*
	 * Make sure only the overlay id, net id, and route table id are
	 * specified.
	 */
	if (!IN6_IS_ADDR_UNSPECIFIED(&rnet->oin_routeraddrv6) ||
	    rnet->oin_routeraddr != INADDR_ANY ||
	    rnet->oin_prefixlen != 0 ||
	    rnet->oin_prefixlenv6 != 0 ||
	    rnet->oin_vlan != 0 ||
	    bcmp(rnet->oin_mac, overlay_macaddr, ETHERADDRL) != 0)
		return (EINVAL);

	if (!overlay_valid_id(rnet->oin_id, sizeof (rnet->oin_id)) ||
	    !overlay_valid_id(rnet->oin_routetbl, sizeof (rnet->oin_routetbl)))
		return (EINVAL);

	net = overlay_hold_net_by_id(orr, rnet->oin_id);
	if (net == NULL)
		return (ENOENT);

	if (strlen(rnet->oin_routetbl) > 0) {
		rtab = overlay_route_tbl_hold_by_id(orr, rnet->oin_routetbl);
		if (rtab == NULL) {
			overlay_net_rele(net);
			return (ENOENT);
		}
	}

	mutex_enter(&net->ont_lock);

	/*
	 * We have a hold from overlay_route_tbl_hold_by_id(), we don't need
	 * it after this, so give the ref to net->ont_routetbl
	 */
	if (net->ont_routetbl != NULL)
		overlay_route_tbl_rele(net->ont_routetbl);
	net->ont_routetbl = rtab;
	mutex_exit(&net->ont_lock);

	overlay_net_rele(net);
	return (0);
}

static int
overlay_router_tbl_copyin(const void *buf, void **outp, size_t *bsize,
    int flags)
{
	overlay_ioc_routetab_t base, *ioc_rtab;

	if (ddi_copyin(buf, &base, sizeof (base), flags & FKIOCTL) != 0)
		return (EFAULT);

	if (!overlay_valid_id(base.oir_id, sizeof (base.oir_id)))
		return (EINVAL);

	*bsize = sizeof (base) +
	    base.oir_count * sizeof (overlay_route_ent_t);
	ioc_rtab = kmem_zalloc(*bsize, KM_SLEEP);

	if (base.oir_count == 0) {
		bcopy(&base, ioc_rtab, sizeof (base));
		goto done;
	}

	if (ddi_copyin(buf, ioc_rtab, *bsize, flags & FKIOCTL) != 0) {
		kmem_free(ioc_rtab, *bsize);
		return (EFAULT);
	}

	if (ioc_rtab->oir_count != base.oir_count) {
		kmem_free(ioc_rtab, *bsize);
		return (EFAULT);
	}

done:
	*outp = ioc_rtab;
	return (0);
}

/*
 * The marker is an opaque value to userland, and is initialized to 0
 * upon start of an iteration request.
 */
#define	OROUTE_ENT_MARKER_F_DONE	0x01
#define	OROUTE_ENT_MARKER_F_V6		0x02
#define	OROUTE_ENT_MARKER_F_ALL \
	(OROUTE_ENT_MARKER_F_DONE|OROUTE_ENT_MARKER_F_V6)

typedef struct overlay_route_ent_marker {
	uint32_t	orm_index;
	uint32_t	orm_flags;
} overlay_route_ent_marker_t;
CTASSERT(sizeof (overlay_route_ent_marker_t) == sizeof (uint64_t));

static int
overlay_route_tbl_get(overlay_router_t *orr, void *buf)
{
	overlay_ioc_routetab_t *ioc_rtab = buf;
	overlay_routetab_t *rtab;
	overlay_route_ent_marker_t *mark;
	overlay_route_ent_t *src, *dst;
	uint_t len;
	int ret = 0;
	uint16_t written = 0;

	mark = (void *)&ioc_rtab->oir_marker;

	if ((mark->orm_flags | OROUTE_ENT_MARKER_F_ALL) !=
	    OROUTE_ENT_MARKER_F_ALL) {
		ioc_rtab->oir_count = 0;
		return (EINVAL);
	}

	if ((mark->orm_flags & OROUTE_ENT_MARKER_F_DONE) != 0) {
		ioc_rtab->oir_count = 0;
		return (0);
	}

	rtab = overlay_route_tbl_hold_by_id(orr, ioc_rtab->oir_id);
	if (rtab == NULL)
		return (ENOENT);

	if ((mark->orm_flags & OROUTE_ENT_MARKER_F_V6) == 0) {
		src = rtab->ort_routev4;
		len = rtab->ort_nroutev4;
	} else {
		src = rtab->ort_routev6;
		len = rtab->ort_nroutev6;
	}

	dst = ioc_rtab->oir_ents;
	while (written < ioc_rtab->oir_count) {
		if (mark->orm_index > len) {
			ret = EINVAL;
			goto done;
		}

		if (mark->orm_index == len) {
			if ((mark->orm_flags & OROUTE_ENT_MARKER_F_V6) != 0) {
				mark->orm_flags |= OROUTE_ENT_MARKER_F_DONE;
				break;
			}

			mark->orm_index = 0;
			mark->orm_flags |= OROUTE_ENT_MARKER_F_V6;

			src = rtab->ort_routev6;
			len = rtab->ort_nroutev6;
			continue;
		}

		bcopy(&src[mark->orm_index++], &dst[written++], sizeof (*dst));
	}

	ioc_rtab->oir_count = written;
done:
	overlay_route_tbl_rele(rtab);
	return (ret);
}

/*
 * Make sure *entp has enough free entries to hold 'amt' new entries in *entp
 */
static int
overlay_rtab_reserve(overlay_route_ent_t **entpp, uint_t nent,
    uint_t *allocp, uint_t amt)
{
	overlay_route_ent_t *newent = NULL;
	uint64_t total = nent + amt;

	if (total > UINT_MAX)
		return (EOVERFLOW);

	if (total <= *allocp)
		return (0);

	total = P2ROUNDUP(total, 4);
	newent = kmem_zalloc(total * sizeof (overlay_route_ent_t), KM_SLEEP);

	if (nent > 0) {
		bcopy(*entpp, newent, nent * sizeof (overlay_route_ent_t));
		kmem_free(*entpp, *allocp * sizeof (overlay_route_ent_t));
	}

	*entpp = newent;
	*allocp = total;

	return (0);
}

static void
overlay_rtab_remove_dups(overlay_route_ent_t *ents, uint_t *nentp, uint_t alloc)
{
	if (*nentp == 0)
		return;

	uint_t i = 0;
	uint_t n = *nentp;

	while (i < n - 1) {
		if (overlay_route_ent_cmp(&ents[i], &ents[i + 1]) != 0) {
			i++;
			continue;
		}

		/*
		 * If the last two entries are dup, just reduce the number
		 * of entries.
		 */
		if (i + 1 == n) {
			n--;
			break;
		}

		/*
		 * ent[i] and ent[i + 1] are duplicates. Shift the entries
		 * starting at ent[i + 1] over one slot.
		 */
		(void) memmove(&ents[i], &ents[i + 1],
		    (n - i - 1) * sizeof (overlay_route_ent_t));
		n--;

		/*
		 * We don't advance i, so we can check i + 1 again in case
		 * there are a string of duplicates (since the entries are
		 * sorted, any duplicates will be contiguous). We could get
		 * more clever and count the run of duplicates to remove
		 * them in a single memmove(), but realistically userland
		 * shouldn't be adding lots of duplicates in a single
		 * request. If they do, they can take the small penalty
		 * doing it in this manner causes.
		 */
	}

	/*
	 * For diagnostic purposes, clear out any entries beyond what's
	 * actually valid.
	 */
	if (n < alloc) {
		bzero(&ents[n], (alloc - n) * sizeof (overlay_route_ent_t));
	}

	*nentp = n;
}

static int
add_rtbl_ents(overlay_routetab_t *rtab, const overlay_route_ent_t *ents,
    uint_t nent)
{
	uint_t nv4, nv6;
	uint_t i;
	int ret;

	nv4 = nv6 = 0;
	for (i = 0; i < nent; i++) {
		const struct sockaddr_in6 *sin6 = &ents[i].ore_target;

		if (IN6_IS_ADDR_V4MAPPED(&ents[i].ore_dest))
			nv4++;
		else
			nv6++;

		if (sin6->sin6_family != AF_INET6)
			return (EINVAL);

		if (!IN6_IS_ADDR_V4MAPPED_ANY(&sin6->sin6_addr) &&
		    !IN6_IS_ADDR_UNSPECIFIED(&sin6->sin6_addr) &&
		    sin6->sin6_port == 0) {
			return (EINVAL);
		}
	}

	mutex_enter(&rtab->ort_lock);

	ret = overlay_rtab_reserve(&rtab->ort_routev4, rtab->ort_nroutev4,
	    &rtab->ort_routev4_alloc, nv4);
	if (ret != 0) {
		mutex_exit(&rtab->ort_lock);
		return (ret);
	}

	ret = overlay_rtab_reserve(&rtab->ort_routev6, rtab->ort_nroutev6,
	    &rtab->ort_routev6_alloc, nv6);
	if (ret != 0) {
		mutex_exit(&rtab->ort_lock);
		return (ret);
	}

	for (i = 0; i < nent; i++) {
		overlay_route_ent_t *dst;

		if (IN6_IS_ADDR_V4MAPPED(&ents[i].ore_dest))
			dst = &rtab->ort_routev4[rtab->ort_nroutev4++];
		else
			dst = &rtab->ort_routev6[rtab->ort_nroutev6++];

		bcopy(&ents[i], dst, sizeof (*dst));
	}

	if (rtab->ort_nroutev4 > 0) {
		qsort(rtab->ort_routev4, rtab->ort_nroutev4,
		    sizeof (overlay_route_ent_t), overlay_route_ent_cmp);
	}

	if (rtab->ort_nroutev6 > 0) {
		qsort(rtab->ort_routev6, rtab->ort_nroutev6,
		    sizeof (overlay_route_ent_t), overlay_route_ent_cmp);
	}

	overlay_rtab_remove_dups(rtab->ort_routev4, &rtab->ort_nroutev4,
	    rtab->ort_routev4_alloc);
	overlay_rtab_remove_dups(rtab->ort_routev6, &rtab->ort_nroutev6,
	    rtab->ort_routev6_alloc);

	mutex_exit(&rtab->ort_lock);
	return (0);
}

static int
overlay_route_tbl_set(overlay_router_t *orr, void *buf)
{
	const overlay_ioc_routetab_t *ioc_rtab = buf;
	overlay_routetab_t *newtbl, *tbl;
	int ret;

	if (!overlay_valid_id(ioc_rtab->oir_id, sizeof (ioc_rtab->oir_id)))
		return (EINVAL);

	newtbl = kmem_cache_alloc(overlay_rtab_cache, KM_SLEEP);

	(void) strlcpy(newtbl->ort_id, ioc_rtab->oir_id,
	    sizeof (newtbl->ort_id));

	/*
	 * Proactively set refcnt to 1 for hold in orr -- if we fail before
	 * adding, we can use overlay_route_tbl_rele() to free.
	 */
	newtbl->ort_refcnt = 1;

	ret = add_rtbl_ents(newtbl, ioc_rtab->oir_ents, ioc_rtab->oir_count);
	if (ret != 0) {
		overlay_route_tbl_rele(newtbl);
		return (ret);
	}

	tbl = overlay_route_tbl_hold_by_id(orr, ioc_rtab->oir_id);

	mutex_enter(&orr->orr_lock);

	/* We already set refcnt to 1 earlier for this */
	list_insert_tail(&orr->orr_routetbls, newtbl);

	/*
	 * If we're replacing an existing table, we need to remove the old
	 * one from orr, and update any references to it by any overlay_net_ts
	 */
	if (tbl != NULL) {
		list_remove(&orr->orr_routetbls, tbl);

		mutex_enter(&tbl->ort_lock);
		mutex_enter(&newtbl->ort_lock);

		/*
		 * We should have at least 2 refs -- the one we took plus
		 * the one from orr->orr_routetbls. Remove the ref for
		 * orr_routetbls.
		 */
		ASSERT3U(tbl->ort_refcnt, >, 1);
		tbl->ort_refcnt--;

		if (orr->orr_def_routetab == tbl) {
			orr->orr_def_routetab = newtbl;

			ASSERT3U(tbl->ort_refcnt, >, 1);
			tbl->ort_refcnt--;
			newtbl->ort_refcnt++;
		}

		if (tbl->ort_refcnt > 1) {
			overlay_net_t *iter;

			for (iter = avl_first(&orr->orr_nets_mac); iter != NULL;
			    iter = AVL_NEXT(&orr->orr_nets_mac, iter)) {
				if (iter->ont_routetbl != tbl)
					continue;

				mutex_enter(&iter->ont_lock);
				iter->ont_routetbl = newtbl;
				mutex_exit(&iter->ont_lock);

				ASSERT3U(tbl->ort_refcnt, >, 1);
				tbl->ort_refcnt--;
				newtbl->ort_refcnt++;

			}
		}
		mutex_exit(&newtbl->ort_lock);
		mutex_exit(&tbl->ort_lock);
	}

	mutex_exit(&orr->orr_lock);

	if (tbl != NULL) {
		/* Our hold from overlay_route_tbl_hold_by_id() */
		overlay_route_tbl_rele(tbl);
	}

	return (ret);
}

static int
overlay_route_tbl_del(overlay_router_t *orr, void *buf)
{
	overlay_ioc_routetab_t *ioc_rtab = buf;
	overlay_routetab_t *rtab;
	uint_t cnt;

	if (!overlay_valid_id(ioc_rtab->oir_id, sizeof (ioc_rtab->oir_id)))
		return (EINVAL);

	rtab = overlay_route_tbl_hold_by_id(orr, ioc_rtab->oir_id);
	if (rtab == NULL)
		return (ENOENT);

	cnt = 2;

	mutex_enter(&orr->orr_lock);
	if (orr->orr_def_routetab == rtab)
		cnt++;

	mutex_enter(&rtab->ort_lock);
	if (rtab->ort_refcnt > cnt) {
		mutex_exit(&rtab->ort_lock);
		mutex_exit(&orr->orr_lock);

		overlay_route_tbl_rele(rtab);
		return (EBUSY);
	}

	if (orr->orr_def_routetab == rtab) {
		orr->orr_def_routetab = NULL;
		rtab->ort_refcnt--;
	}

	list_remove(&orr->orr_routetbls, rtab);
	rtab->ort_refcnt--;

	mutex_exit(&rtab->ort_lock);
	mutex_exit(&orr->orr_lock);

	overlay_route_tbl_rele(rtab);
	return (0);
}

static int
overlay_route_tbl_set_default(overlay_router_t *orr, void *buf)
{
	overlay_ioc_routetab_t *ioc_rtab = buf;
	overlay_routetab_t *rtab;

	if (ioc_rtab->oir_marker != 0 || ioc_rtab->oir_count != 0)
		return (EINVAL);

	if (!overlay_valid_id(ioc_rtab->oir_id, sizeof (ioc_rtab->oir_id)))
		return (EINVAL);

	rtab = overlay_route_tbl_hold_by_id(orr, ioc_rtab->oir_id);
	if (rtab == NULL && ioc_rtab->oir_id[0] != '\0')
		return (ENOENT);

	mutex_enter(&orr->orr_lock);
	if (orr->orr_def_routetab != NULL)
		overlay_route_tbl_rele(orr->orr_def_routetab);

	orr->orr_def_routetab = rtab;

	mutex_enter(&rtab->ort_lock);
	rtab->ort_refcnt++;
	mutex_exit(&rtab->ort_lock);

	mutex_exit(&orr->orr_lock);

	return (0);
}

static int
overlay_route_tbl_addent(overlay_router_t *orr, void *buf)
{
	overlay_ioc_routetab_t *ioc_rtab = buf;
	overlay_routetab_t *rtab;
	int ret;

	if (ioc_rtab->oir_marker != 0)
		return (EINVAL);

	if (!overlay_valid_id(ioc_rtab->oir_id, sizeof (ioc_rtab->oir_id)))
		return (EINVAL);

	rtab = overlay_route_tbl_hold_by_id(orr, ioc_rtab->oir_id);
	if (rtab == NULL)
		return (ENOENT);

	ret = add_rtbl_ents(rtab, ioc_rtab->oir_ents, ioc_rtab->oir_count);

	overlay_route_tbl_rele(rtab);
	return (ret);
}

static int
del_ent(overlay_route_ent_t *ents, uint_t *nentp,
    const overlay_route_ent_t *cmp)
{
	overlay_route_ent_t *tgt;
	ptrdiff_t idx;

	tgt = bsearch(cmp, ents, *nentp, sizeof (*cmp), overlay_route_ent_cmp);
	if (tgt == NULL)
		return (ENOENT);

	idx = tgt - ents;
	(void) memmove(tgt + 1, tgt, *nentp - idx - 1);
	*nentp = *nentp - 1;

	return (0);
}

static int
overlay_route_tbl_delent(overlay_router_t *orr, void *buf)
{
	overlay_ioc_routetab_t *ioc_rtab = buf;
	overlay_routetab_t *rtab;
	overlay_route_ent_t *ent;
	int ret;

	/*
	 * We currently only allow deletions one at a time -- if we want to
	 * allow batch deletions, we probably want a way to respond with
	 * which (if any) entries weren't found.
	 */
	if (ioc_rtab->oir_marker != 0 || ioc_rtab->oir_count != 1)
		return (EINVAL);

	if (!overlay_valid_id(ioc_rtab->oir_id, sizeof (ioc_rtab->oir_id)))
		return (EINVAL);

	ent = ioc_rtab->oir_ents;

	rtab = overlay_route_tbl_hold_by_id(orr, ioc_rtab->oir_id);

	mutex_enter(&rtab->ort_lock);
	if (IN6_IS_ADDR_V4MAPPED(&ent->ore_dest)) {
		ret = del_ent(rtab->ort_routev4, &rtab->ort_nroutev4, ent);
	} else {
		ret = del_ent(rtab->ort_routev6, &rtab->ort_nroutev6, ent);
	}
	mutex_exit(&rtab->ort_lock);

	overlay_route_tbl_rele(rtab);
	return (ret);
}

static int
overlay_routetab_iter_copyin(const void *buf, void **outp, size_t *bsize,
    int flags)
{
	overlay_ioc_rtab_iter_t base, *ioc_rtab;

	if (ddi_copyin(buf, &base, sizeof (base), flags & FKIOCTL) != 0)
		return (EFAULT);

	*bsize = sizeof (base) +
	    base.oiri_count * sizeof (overlay_ioc_routetab_t);
	ioc_rtab = kmem_zalloc(*bsize, KM_SLEEP);

	if (base.oiri_count == 0) {
		bcopy(&base, ioc_rtab, sizeof (base));
		goto done;
	}

	if (ddi_copyin(buf, ioc_rtab, *bsize, flags & FKIOCTL) != 0) {
		kmem_free(ioc_rtab, *bsize);
		return (EFAULT);
	}

	if (ioc_rtab->oiri_count != base.oiri_count) {
		kmem_free(ioc_rtab, *bsize);
		return (EFAULT);
	}

done:
	*outp = ioc_rtab;
	return (0);
}

static int
overlay_routetab_iter(overlay_router_t *orr, void *buf)
{
	overlay_ioc_rtab_iter_t *iter = buf;
	overlay_routetab_t *start =
	    (overlay_routetab_t *)(uintptr_t)iter->oiri_marker;
	overlay_routetab_t *rtab;
	size_t n;

	/*
	 * iter->oiri_marker is either 0 to start iterating through the
	 * route tables, 1 when we've finished, or we've set it (on the
	 * previous ioctl) to the address of the overlay_routetab_t to
	 * start at
	 */
	if (iter->oiri_marker == 1) {
		iter->oiri_count = 0;
		return (0);
	}

	mutex_enter(&orr->orr_lock);

	/*
	 * Even though we have the address to start with, we don't
	 * want to blindly trust the value, we find it, or return an error.
	 */
	rtab = list_head(&orr->orr_routetbls);
	if (start != NULL) {
		while (rtab != NULL && rtab != start)
			rtab = list_next(&orr->orr_routetbls, rtab);

		if (rtab == NULL) {
			mutex_exit(&orr->orr_lock);
			return (EINVAL);
		}
	}

	n = 0;
	while (rtab != NULL && n < iter->oiri_count) {
		overlay_ioc_routetab_t *ioc_rtab = &iter->oiri_rtabs[n];

		bzero(ioc_rtab, sizeof (*ioc_rtab));
		ioc_rtab->oir_count = rtab->ort_nroutev4 + rtab->ort_nroutev6;
		(void) strlcpy(ioc_rtab->oir_id, rtab->ort_id,
		    sizeof (ioc_rtab->oir_id));

		n++;
		rtab = list_next(&orr->orr_routetbls, rtab);
	}
	mutex_exit(&orr->orr_lock);

	iter->oiri_marker = (rtab == NULL) ? 1 : (uintptr_t)rtab;
	iter->oiri_count = n;
	return (0);
}

int
overlay_router_open(dev_t *devp, int flags, int otype, cred_t *credp)
{
	if (secpolicy_dl_config(credp) != 0)
		return (EPERM);

	if (getminor(*devp) != OVERLAY_ROUTER_MINOR)
		return (ENXIO);

	if (otype & OTYP_BLK)
		return (EINVAL);

	if (flags & ~(FREAD | FWRITE | FEXCL | FOFFMAX))
		return (EINVAL);

	/* We don't allow O_EXCL for the router device.. for now at least */
	if ((flags & FEXCL) != 0)
		return (EINVAL);

	if (!(flags & FREAD) && !(flags & FWRITE))
		return (EINVAL);

	if (crgetzoneid(credp) != GLOBAL_ZONEID)
		return (EPERM);

	return (0);
}

int
overlay_router_close(dev_t dev, int flags, int otype, cred_t *credp)
{
	/*
	 * Currently nothing to do (i.e. this function left intentionally empty)
	 */
	return (0);
}

static overlay_router_ioctl_t overlay_router_ioctab[] = {
	{ OVERLAY_ROUTER_NET_CREATE, OFF_RW,
	    NULL, overlay_router_net_create,
	    sizeof (overlay_ioc_net_t) },
	{ OVERLAY_ROUTER_NET_DELETE, OFF_RW,
	    NULL, overlay_router_net_delete,
	    sizeof (overlay_ioc_net_t) },
	{ OVERLAY_ROUTER_NET_DELETE_ALL, OFF_RW,
	    NULL, overlay_router_net_delete_all,
	    sizeof (overlay_ioc_net_t) },
	{ OVERLAY_ROUTER_NET_GET, OFF_COPYOUT,
	    NULL, overlay_router_net_get,
	    sizeof (overlay_ioc_net_t) },
	{ OVERLAY_ROUTER_NET_ITER, OFF_COPYOUT,
	    overlay_router_iter_copyin, overlay_router_net_iter,
	    sizeof (overlay_ioc_net_iter_t) },
	{ OVERLAY_ROUTER_NET_SET_ROUTETBL, OFF_RW,
	    NULL, overlay_router_net_set_routetbl,
	    sizeof (overlay_ioc_net_t) },

	{ OVERLAY_ROUTETBL_GET, OFF_COPYOUT,
	    overlay_router_tbl_copyin, overlay_route_tbl_get,
	    sizeof (overlay_ioc_routetab_t) },
	{ OVERLAY_ROUTETBL_SET, OFF_RW,
	    overlay_router_tbl_copyin, overlay_route_tbl_set,
	    sizeof (overlay_ioc_routetab_t) },
	{ OVERLAY_ROUTETBL_REMOVE, OFF_RW,
	    overlay_router_tbl_copyin, overlay_route_tbl_del,
	    sizeof (overlay_ioc_routetab_t) },
	{ OVERLAY_ROUTETBL_SET_DEFAULT, OFF_RW,
	    overlay_router_tbl_copyin, overlay_route_tbl_set_default,
	    sizeof (overlay_ioc_routetab_t) },
#if 0
	{ OVERLAY_ROUTETBL_FLUSH, },
#endif

	{ OVERLAY_ROUTETBL_ITER, OFF_COPYOUT,
	    overlay_routetab_iter_copyin, overlay_routetab_iter,
	    sizeof (overlay_ioc_routetab_t) },
	{ OVERLAY_ROUTETBL_ADDENT, OFF_RW,
	    overlay_router_tbl_copyin, overlay_route_tbl_addent,
	    sizeof (overlay_ioc_routetab_t) },
	{ OVERLAY_ROUTETBL_DELENT, OFF_RW,
	    overlay_router_tbl_copyin, overlay_route_tbl_delent,
	    sizeof (overlay_ioc_rtab_iter_t) },

#if 0
	{ OVERLAY_ROUTETBL_FLUSHENT, },
#endif

	{ 0 }
};

int
overlay_router_ioctl(dev_t dev, int cmd, intptr_t arg, int mode, cred_t *credp,
    int *rvalp)
{
	overlay_router_ioctl_t *ioc;

	if (secpolicy_dl_config(credp) != 0)
		return (EPERM);

	/*
	 * This whole thing is a private interface, we demand callers are
	 * 64-bit (it's not worth the effort to support 32-bit callers
	 * at this time).
	 */
	if (ddi_model_convert_from(mode & FMODELS) == DDI_MODEL_ILP32)
		return (ENOTSUP);

	for (ioc = overlay_router_ioctab; ioc->ori_cmd != 0; ioc++) {
		overlay_router_t *orr;
		overlay_router_ioc_hdr_t *hdr;
		int ret;
		caddr_t buf = NULL;
		size_t bufsize = 0;

		if (ioc->ori_cmd != cmd)
			continue;

		if (((ioc->ori_flags & OFF_RW) != 0) && ((mode & FWRITE) == 0))
			return (EBADF);

		ASSERT3U(ioc->ori_size, >, 0);

		if (ioc->ori_copyin == NULL) {
			bufsize = ioc->ori_size;
			buf = kmem_alloc(bufsize, KM_SLEEP);
			if (ddi_copyin((void *)(uintptr_t)arg, buf, bufsize,
			    mode & FKIOCTL) != 0) {
				kmem_free(buf, bufsize);
				return (EFAULT);
			}
		} else {
			ret = ioc->ori_copyin((void *)(uintptr_t)arg,
			    (void **)&buf, &bufsize, mode);
			if (ret != 0)
				return (ret);
		}

		VERIFY3U(bufsize, >=, sizeof (*hdr));
		hdr = (overlay_router_ioc_hdr_t *)buf;
		ret = overlay_router_hold_by_dlid(hdr->orih_linkid, &orr);
		if (ret != 0)
			return (ret);

		ret = ioc->ori_func(orr, buf);
		overlay_router_rele(orr);

		if ((ioc->ori_flags & OFF_COPYOUT) != 0) {
			ret = ddi_copyout(buf, (void *)(uintptr_t)arg,
			    bufsize, mode & FKIOCTL);
			if (ret != 0)
				ret = EFAULT;
		}

		kmem_free(buf, bufsize);
		return (ret);
	}

	return (ENXIO);
}

static overlay_routetab_t *
overlay_get_rtab(overlay_router_t *orr, overlay_net_t *ont)
{
	overlay_routetab_t *rtab;

	mutex_enter(&ont->ont_lock);
	rtab = ont->ont_routetbl;
	if (rtab != NULL) {
		mutex_enter(&rtab->ort_lock);
		rtab->ort_refcnt++;
		mutex_exit(&rtab->ort_lock);
		mutex_exit(&ont->ont_lock);
		return (rtab);
	}
	mutex_exit(&ont->ont_lock);

	mutex_enter(&orr->orr_lock);
	rtab = orr->orr_def_routetab;
	if (rtab == NULL) {
		mutex_exit(&orr->orr_lock);
		return (NULL);
	}

	mutex_enter(&rtab->ort_lock);
	rtab->ort_refcnt++;
	mutex_exit(&rtab->ort_lock);
	mutex_exit(&orr->orr_lock);

	return (rtab);
}

static inline uint16_t
overlay_get16(unsigned char **pp)
{
	unsigned char *p = *pp;
	uint16_t val;

	val = *p++ << 8;
	val |= *p++;

	*pp = p;
	return (val);
}

static inline void
overlay_put8(mblk_t *mp, uint8_t val)
{
	ASSERT3U(MBLKTAIL(mp), >=, sizeof (uint8_t));

	*(mp->b_wptr) = val;
	mp->b_wptr++;
}

static inline void
overlay_put16(mblk_t *mp, uint16_t val)
{
	/* Since we're constructing the packet, these should be correct */
	ASSERT3U(MBLKTAIL(mp), >=, sizeof (uint16_t));
	ASSERT(IS_P2ALIGNED(mp->b_wptr, sizeof (uint16_t)));

	uint16_t *p = (uint16_t *)mp->b_wptr;

	*p = val;
	mp->b_wptr += sizeof (uint16_t);
}

static inline void
overlay_put32(mblk_t *mp, uint32_t val)
{
	/*
	 * Since we're constructing the packet, these should be correct
	 * Because of the ethernet header, the best we can assume is
	 * 16-bit alignment.
	 */
	ASSERT3U(MBLKTAIL(mp), >=, sizeof (uint16_t));
	ASSERT(IS_P2ALIGNED(mp->b_wptr, sizeof (uint16_t)));

	bcopy(&val, mp->b_wptr, sizeof (val));
	mp->b_wptr += sizeof (val);
}

static inline void
overlay_put_mac(mblk_t *mp, const uint8_t *mac)
{
	ASSERT3U(MBLKTAIL(mp), >=, ETHERADDRL);

	bcopy(mac, mp->b_wptr, ETHERADDRL);
	mp->b_wptr += ETHERADDRL;
}

static inline void
overlay_put_ip(mblk_t *mp, in_addr_t ip)
{
	ASSERT3U(MBLKTAIL(mp), >=, sizeof (in_addr_t));

	bcopy(&ip, mp->b_wptr, sizeof (ip));
	mp->b_wptr += sizeof (ip);
}

static inline void
overlay_put_ip6(mblk_t *mp, const struct in6_addr *addr)
{
	ASSERT3U(MBLKTAIL(mp), >=, sizeof (struct in6_addr));

	bcopy(addr, mp->b_wptr, sizeof (*addr));
	mp->b_wptr += sizeof (*addr);
}

static inline void
overlay_put_eth_vlan(mblk_t *mp, const uint8_t *dst, const uint8_t *src,
    uint16_t tci, uint16_t etype)
{
	overlay_put_mac(mp, dst);
	overlay_put_mac(mp, src);
	if (tci > 0) {
		overlay_put16(mp, htons(ETHERTYPE_VLAN));
		overlay_put16(mp, htons(tci));
	}
	overlay_put16(mp, htons(etype));
}

/* The size of an ARP packet for ethernet */
#define	ARP_ETHER_SIZE	28

/* The ARP hardware type for ethernet */
#define	ARP_HW_ETHER	1

/*
 * Handle ARP requests for the router MAC. Returns B_TRUE if we handled the
 * ARP request, B_FALSE if we did not (implying pkt should undergo further
 * handling).
 *
 * In the future, we could look to use the overlay_target L3->L2 mappings
 * to satisify ARP requests for known targets (and only drop out to varpd
 * for unknown targets), but for now, we just handle ARP for the router
 * IP/MAC.
 */
boolean_t
overlay_router_arp(overlay_dev_t *odd, overlay_net_t *ont, overlay_pkt_t *pkt)
{
	unsigned char *ptr;
	unsigned char *src_hwaddr;
	mblk_t *resp = NULL;
	in_addr_t src_ip, tgt_ip;

	ASSERT3P(ont, !=, NULL);

	/*
	 * Must be sent to either ethernet broadcast address or our
	 * router address to handle it.
	 */
	if (bcmp(overlay_bcast, pkt->op_mhi.mhi_daddr, ETHERADDRL) != 0 &&
	    bcmp(ont->ont_mac, pkt->op_mhi.mhi_daddr, ETHERADDRL) != 0)
		return (B_FALSE);

	/*
	 * Some sanity checks:
	 *
	 * - Hardware type is ARP_HW_ETHER
	 * - Protocol type is IPv4 (ETHERTYPE_IP)
	 * - Hardware length is ETHERADDRL
	 * - Protocol length is sizeof (in_addr_t)
	 * - Operation is ARP_REQUEST
	 *
	 * overlay_pkt_init() already guarantees us a 28 byte, contiguous
	 * in ram packet when it's ARP, so we can traverse this safely.
	 */
	ptr = pkt->op2_u.op2_char;
	if (overlay_get16(&ptr) != ARP_HW_ETHER ||
	    overlay_get16(&ptr) != ETHERTYPE_IP ||
	    *ptr++ != ETHERADDRL || *ptr++ != sizeof (in_addr_t) ||
	    overlay_get16(&ptr) != ARPOP_REQUEST)
		return (B_FALSE);

	src_hwaddr = ptr;
	ptr += ETHERADDRL;

	bcopy(ptr, &src_ip, sizeof (in_addr_t));
	ptr += sizeof (in_addr_t);

	/* skip over the target ethernet address */
	ptr += ETHERADDRL;

	bcopy(ptr, &tgt_ip, sizeof (in_addr_t));
	ptr += sizeof (in_addr_t);

	if (tgt_ip != ont->ont_routeraddr)
		return (B_FALSE);

	resp = allocb(pkt->op_mhi.mhi_hdrsize + ARP_ETHER_SIZE, 0);
	if (resp == NULL)
		return (B_TRUE);

	/*
	 * For our response, the destination mac is src_hwaddr (the originator
	 * of the request), and the source mac is our router mac (mac). The
	 * tci (really just vlan) comes from the source packet, and we're of
	 * course sending an ARP packet.
	 */
	overlay_put_eth_vlan(resp, pkt->op_mhi.mhi_saddr, ont->ont_mac,
	    pkt->op_mhi.mhi_tci, ETHERTYPE_ARP);

	/*
	 * Construct our ARP response. Since this is a response, the
	 * sender is the router and the target is the originator of the
	 * ARP request.
	 */
	overlay_put16(resp, htons(ARP_HW_ETHER));	/* hardware type */
	overlay_put16(resp, htons(ETHERTYPE_IP));	/* protocol type */
	overlay_put8(resp, ETHERADDRL);			/* hw address length */
	overlay_put8(resp, sizeof (in_addr_t));	/* protocol address length */
	overlay_put16(resp, htons(ARPOP_REPLY));	/* operation */
	overlay_put_mac(resp, ont->ont_mac);		/* sender hw address */
	overlay_put_ip(resp, tgt_ip);		/* sender protocol address */
	overlay_put_mac(resp, src_hwaddr);	/* target hw address */
	overlay_put_ip(resp, src_ip);		/* target protocol address */

	mutex_enter(&odd->odd_lock);
	overlay_io_start(odd, OVERLAY_F_IN_RX);
	mutex_exit(&odd->odd_lock);

	mac_rx(odd->odd_mh, NULL, resp);

	mutex_enter(&odd->odd_lock);
	overlay_io_done(odd, OVERLAY_F_IN_RX);
	mutex_exit(&odd->odd_lock);

	return (B_TRUE);
}

boolean_t
overlay_router_ndp(overlay_dev_t *odd, overlay_net_t *ont, overlay_pkt_t *pkt)
{
	nd_neighbor_solicit_t *nd;
	size_t len = pkt->op_l3len;

	ASSERT3P(ont, !=, NULL);
	ASSERT3U(pkt->op_l3proto, ==, IPPROTO_ICMPV6);

	if (IN6_IS_ADDR_UNSPECIFIED(&ont->ont_routeraddrv6))
		return (B_FALSE);

	if (!IN6_IS_ADDR_MC_SOLICITEDNODE(&pkt->op_dstaddr) &&
	    !IN6_IS_ADDR_MC_LINKLOCAL(&pkt->op_dstaddr))
		return (B_FALSE);

	nd = (nd_neighbor_solicit_t *)pkt->op3_u.op3_char;

	if (nd->nd_ns_type != ND_NEIGHBOR_SOLICIT && nd->nd_ns_code != 0)
		return (B_FALSE);

	if (len < sizeof (*nd))
		return (B_FALSE);

	if (IN6_IS_ADDR_MULTICAST(&nd->nd_ns_target) ||
	    IN6_IS_ADDR_V4MAPPED(&nd->nd_ns_target) ||
	    IN6_IS_ADDR_LOOPBACK(&nd->nd_ns_target))
		return (B_FALSE);

	uint8_t *eth = NULL;
	nd_opt_hdr_t *opt = (nd_opt_hdr_t *)(nd + 1);

	len -= sizeof (*nd);
	while (len >= sizeof (*opt)) {
		if (opt->nd_opt_len == 0)
			return (B_FALSE);

		if (opt->nd_opt_type == ND_OPT_SOURCE_LINKADDR) {
			eth = (uint8_t *)((uintptr_t)opt +
			    sizeof (nd_opt_hdr_t));
		}
		len -= opt->nd_opt_len * 8;
		opt = (nd_opt_hdr_t *)((uintptr_t)opt +
		    opt->nd_opt_len * 8);
	}

	if (eth == NULL)
		return (B_FALSE);

	if (!IN6_ARE_ADDR_EQUAL(&ont->ont_routeraddrv6, &nd->nd_ns_target))
		return (B_FALSE);

	/* It's for us, construct a reply */
	mblk_t *resp = allocb(ETHERMAX + VLAN_TAGSZ, 0);

	if (resp == NULL)
		return (B_TRUE);

	overlay_put_eth_vlan(resp, pkt->op_mhi.mhi_saddr, ont->ont_mac,
	    pkt->op_mhi.mhi_tci, ETHERTYPE_IPV6);

	ip6_t *ip6h = (ip6_t *)resp->b_wptr;

	/*
	 * Write the IPv6 header out. Destination IP is the source IP from
	 * the request, and the source IP is the router IP.
	 */
	bcopy(pkt->op2_u.op2_ipv6, ip6h, sizeof (*ip6h));
	bcopy(&ont->ont_routeraddrv6, &ip6h->ip6_src, sizeof (struct in6_addr));
	bcopy(&pkt->op2_u.op2_ipv6->ip6_src, &ip6h->ip6_dst,
	    sizeof (struct in6_addr));
	ip6h->ip6_nxt = IPPROTO_ICMPV6;
	resp->b_wptr += sizeof (*ip6h);

	nd_neighbor_advert_t *na = (nd_neighbor_advert_t *)resp->b_wptr;

	bzero(na, sizeof (*na));
        na->nd_na_type = ND_NEIGHBOR_ADVERT;
        na->nd_na_code = 0;
        /*
         * RFC 4443 defines that we should set the checksum to zero before we
         * calculate the checksumat we should set the checksum to zero before we
         * calculate it.
         */
        na->nd_na_cksum = 0;
        /*
         * The header <netinet/icmp6.h> has already transformed this
         * into the appropriate host order. Don't use htonl.
         */
        na->nd_na_flags_reserved = ND_NA_FLAG_SOLICITED | ND_NA_FLAG_OVERRIDE;
	bcopy(&ont->ont_routeraddrv6, &na->nd_na_target,
            sizeof (struct in6_addr));
	resp->b_wptr += sizeof (*na);

	opt = (nd_opt_hdr_t *)resp->b_wptr;
        opt->nd_opt_type = ND_OPT_TARGET_LINKADDR;
        opt->nd_opt_len = 1;
	resp->b_wptr += sizeof (*opt);

	overlay_put_mac(resp, ont->ont_mac);

	/* Set the IPv6 length */
	len = (uintptr_t)resp->b_wptr - (uintptr_t)na;
	ip6h->ip6_plen = htons(len);

	/*
	 * Calculate the IPv6 checksum. As nice it would be to re-use the
	 * existing in-kernel IPv6 checksum code, it requires some additional
	 * structures. Maybe in the future we can adjust things to use it.
	 */
	uint16_t *v;
	uint32_t sum = 0;

	v = (uint16_t *)&ip6h->ip6_src;
	for (size_t i = 0; i < sizeof (struct in6_addr); i +=2, v++)
		sum += *v;

	v = (uint16_t *)&ip6h->ip6_dst;
	for (size_t i = 0; i < sizeof (struct in6_addr); i +=2, v++)
		sum += *v;

	sum += ip6h->ip6_plen;

#ifdef _BIG_ENDIAN
	sum += IPPROTO_ICMPV6;
#else
	sum += IPPROTO_ICMPV6 << 8;
#endif

	v = (uint16_t *)na;
	for (size_t i = 0; i < len; i += 2, v++)
		sum += *v;

	while ((sum >> 16) != 0)
		sum = (sum & 0xffff) + (sum >> 16);

	sum &= 0xffff;
        na->nd_na_cksum = ~sum & 0xffff;

	mutex_enter(&odd->odd_lock);
	overlay_io_start(odd, OVERLAY_F_IN_RX);
	mutex_exit(&odd->odd_lock);

	mac_rx(odd->odd_mh, NULL, resp);

	mutex_enter(&odd->odd_lock);
	overlay_io_done(odd, OVERLAY_F_IN_RX);

	return (B_TRUE);
}

static uint8_t
overlay_pkt_hash(overlay_pkt_t *pkt)
{
	uint32_t hash = 0;

	hash ^= pkt->op_srcaddr.s6_addr32[0] ^ pkt->op_dstaddr.s6_addr32[0];
	hash ^= pkt->op_srcaddr.s6_addr32[1] ^ pkt->op_dstaddr.s6_addr32[1];
	hash ^= pkt->op_srcaddr.s6_addr32[2] ^ pkt->op_dstaddr.s6_addr32[2];
	hash ^= pkt->op_srcaddr.s6_addr32[3] ^ pkt->op_dstaddr.s6_addr32[3];
	hash ^= ((uint32_t)pkt->op_dstport) << 16 | pkt->op_srcport;

	uint8_t *p = (uint8_t *)&hash;

	return (p[0] ^ p[1] ^ p[2] ^ p[3] ^ pkt->op_l3proto);
}

static boolean_t
overlay_router_get_target(overlay_routetab_t *ort, overlay_pkt_t *pkt,
    struct sockaddr *addr, socklen_t *slenp)
{
	const struct in6_addr *dst = &pkt->op_dstaddr;
	overlay_route_ent_t *ents, **matches;
	uint_t nents, matchlen, nmatch;

	if (IN6_IS_ADDR_V4MAPPED(dst)) {
		ents = ort->ort_routev4;
		nents = ort->ort_nroutev4;
	} else {
		ents = ort->ort_routev6;
		nents = ort->ort_nroutev6;
	}

	if (nents == 0)
		return (B_FALSE);

	/*
	 * We assume the size of a given overlay route table should be
	 * rather small, such that holding an array of pointers equal to
	 * the number of entries (the largest possible number of matches)
	 * shouldn't be too terrible.
	 *
	 * For now we just check the entire table. In the future, we may
	 * want to store the route tables in a more efficient manner (e.g.
	 * some form of radix tree), but this should be good enough for now.
	 * The way we store route table entries is an implementation detail
	 * and not even the ioctl interfaces depend on it, so it shouldn't
	 * too burdensome to swap it out in the future if necessary.
	 */
	matches = kmem_zalloc(nents * sizeof (overlay_route_ent_t *),
	    KM_NOSLEEP | KM_NORMALPRI);
	if (matches == NULL)
		return (B_FALSE);

	matchlen = 0;
	nmatch = 0;
	for (uint_t i = 0; i < nents; i++) {
		if (!IN6_ARE_PREFIXEDADDR_EQUAL(&ents[i].ore_dest, dst,
		    ents[i].ore_prefixlen)) {
			continue;
		}

		/*
		 * If the match is a shorter match then the longest one so
		 * far, skip it.
		 */
		if (ents[i].ore_prefixlen < matchlen)
			continue;

		/*
		 * If we have a longer match, reset the number of matches,
		 * and update our longest prefix match length.
		 */
		if (ents[i].ore_prefixlen > matchlen) {
			matchlen = ents[i].ore_prefixlen;
			nmatch = 0;
		}

		matches[nmatch++] = &ents[i];
	}

	if (nmatch == 0) {
		kmem_free(matches, nents * sizeof (overlay_route_ent_t *));
		return (B_FALSE);
	}

	uint_t idx = (nmatch > 1) ? overlay_pkt_hash(pkt) % nmatch : 0;
	struct sockaddr_in6 *sin6_match = &matches[idx]->ore_target;

	VERIFY3S(sin6_match->sin6_family, ==, AF_INET6);

	if (IN6_IS_ADDR_V4MAPPED(&sin6_match->sin6_addr)) {
		struct sockaddr_in *sin = (struct sockaddr_in *)addr;

		sin->sin_family = AF_INET;
		IN6_V4MAPPED_TO_INADDR(&sin6_match->sin6_addr, &sin->sin_addr);
		sin->sin_port = sin6_match->sin6_port;
		*slenp = sizeof (struct sockaddr_in);
	} else {
		bcopy(sin6_match, addr, sizeof (*sin6_match));
		*slenp = sizeof (*sin6_match);
	}

	kmem_free(matches, nents * sizeof (overlay_route_ent_t *));
	return (B_TRUE);
}

static inline boolean_t
is_target_local(const struct sockaddr *sa)
{
	const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
	const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;

	switch (sa->sa_family) {
	case AF_INET:
		if (sin->sin_addr.s_addr == INADDR_ANY)
			return (B_TRUE);
		break;
	case AF_INET6:
		if (IN6_IS_ADDR_UNSPECIFIED(&sin6->sin6_addr))
			return (B_TRUE);
		break;
	default:
		cmn_err(CE_PANIC, "unexpected address family value");
	}

	return (B_FALSE);
}

int
overlay_route(overlay_dev_t *odd, overlay_net_t *ont, overlay_pkt_t *pkt,
    struct sockaddr *addr, socklen_t *lenp)
{
	overlay_router_t *orr = odd->odd_router;
	overlay_routetab_t *rtab = NULL;
	overlay_target_t *ott = NULL;
	int ret = 0;

	/*
	 * If we're routing, the destination MAC and vlan of the packet
	 * should agree, otherwise we drop the packet.
	 */
	if (OPKT_VLAN(pkt) != ont->ont_vlan)
		return (OVERLAY_TARGET_DROP);

	switch (pkt->op_mhi.mhi_bindsap) {
	case ETHERTYPE_IP:
	case ETHERTYPE_IPV6:
		break;
	default:
		/*
		 * Drop any non IPv4 or IPv6 packets directed to the router
		 * MAC.
		 */
		return (OVERLAY_TARGET_DROP);
	}

	ott = odd->odd_target;
	if (ott == NULL)
		return (OVERLAY_TARGET_DROP);

	/*
	 * No routing table, and the packet was sent to the router MAC,
	 * we drop.
	 */
	rtab = overlay_get_rtab(orr, ont);
	if (rtab == NULL)
		return (OVERLAY_TARGET_DROP);

	/*
	 * Find the target address+port for this packet in the routing table.
	 * If no target was found, we drop the packet.
	 */
	if (!overlay_router_get_target(rtab, pkt, addr, lenp)) {
		ret = OVERLAY_TARGET_DROP;
		goto done;
	}

	/*
	 * If the target is the 'local' destination (the target is the
	 * any/unspecified address, we do a VL3->VL2 lookup and route within
	 * the vnet.
	 */
	if (is_target_local(addr)) {
		ret = overlay_target_lookup(odd, pkt, B_TRUE, addr, lenp);
	}

	/* XXX: Any additional handling (e.g. NAT, ...) would go here */

done:
	overlay_route_tbl_rele(rtab);
	return (ret);
}

static int
overlay_cmp_net_vlan(const void *a, const void *b)
{
	const overlay_net_t *l = a;
	const overlay_net_t *r = b;

	if (l->ont_vlan < r->ont_vlan)
		return (-1);
	if (l->ont_vlan > r->ont_vlan)
		return (1);
	return (0);
}

static int
overlay_cmp_net_mac(const void *a, const void *b)
{
	const overlay_net_t *l = a;
	const overlay_net_t *r = b;

	for (uint_t i = 0; i < ETHERADDRL; i++) {
		if (l->ont_mac[i] > r->ont_mac[i])
			return (1);
		if (l->ont_mac[i] < r->ont_mac[i])
			return (-1);
	}

	return (0);
}

static int
overlay_cmp_net_v4(const void *a, const void *b)
{
	const overlay_net_t *l_net = a;
	const overlay_net_t *r_net = b;
	const uint32_t l = ntohl((uint32_t)l_net->ont_net);
	const uint32_t r = ntohl((uint32_t)r_net->ont_net);

	if (l < r)
		return (-1);
	if (l > r)
		return (1);
	return (0);
}

static int
overlay_cmp_net_v6(const void *a, const void *b)
{
	const overlay_net_t *l = a;
	const overlay_net_t *r = b;
	const struct in6_addr *laddr = &l->ont_netv6;
	const struct in6_addr *raddr = &r->ont_netv6;

	for (uint_t i = 0; i < sizeof (struct in6_addr); i++) {
		if (laddr->s6_addr[i] < raddr->s6_addr[i])
			return (-1);
		if (laddr->s6_addr[i] > raddr->s6_addr[i])
			return (1);
	}
	return (0);
}

static int
overlay_router_cache_ctor(void *buf, void *arg __unused, int kmflags __unused)
{
	overlay_router_t *orr = buf;

	bzero(orr, sizeof (*orr));
	mutex_init(&orr->orr_lock, NULL, MUTEX_DRIVER, NULL);

	list_create(&orr->orr_routetbls,
	    sizeof (overlay_routetab_t),
	    offsetof(overlay_routetab_t, ort_link));

	avl_create(&orr->orr_nets_vlan, overlay_cmp_net_vlan,
	    sizeof (overlay_net_t), offsetof(overlay_net_t, ont_node_vlan));
	avl_create(&orr->orr_nets_mac, overlay_cmp_net_mac,
	    sizeof (overlay_net_t), offsetof(overlay_net_t, ont_node_mac));
	avl_create(&orr->orr_nets_v4, overlay_cmp_net_v4,
	    sizeof (overlay_net_t), offsetof(overlay_net_t, ont_node_v4));
	avl_create(&orr->orr_nets_v6, overlay_cmp_net_v6,
	    sizeof (overlay_net_t), offsetof(overlay_net_t, ont_node_v6));

	return (0);
}

static void
overlay_router_cache_dtor(void *buf, void *arg __unused)
{
	overlay_router_t *orr = buf;

	avl_destroy(&orr->orr_nets_v6);
	avl_destroy(&orr->orr_nets_v4);
	avl_destroy(&orr->orr_nets_mac);
	avl_destroy(&orr->orr_nets_vlan);
	list_destroy(&orr->orr_routetbls);

	mutex_destroy(&orr->orr_lock);
}

static int
overlay_net_cache_ctor(void *buf, void *arg __unused, int kmflags __unused)
{
	overlay_net_t *ont = buf;

	bzero(ont, sizeof (*ont));
	mutex_init(&ont->ont_lock, NULL, MUTEX_DRIVER, NULL);
	return (0);
}

static void
overlay_net_cache_dtor(void *buf, void *arg __unused)
{
	overlay_net_t *ont = buf;

	mutex_destroy(&ont->ont_lock);
}

static int
overlay_rtab_cache_ctor(void *buf, void *arg __unused, int kmflags __unused)
{
	overlay_routetab_t *ort = buf;

	bzero(ort, sizeof (*ort));
	mutex_init(&ort->ort_lock, NULL, MUTEX_DRIVER, NULL);
	return (0);
}

static void
overlay_rtab_cache_dtor(void *buf, void *arg __unused)
{
	overlay_routetab_t *ort = buf;

	mutex_destroy(&ort->ort_lock);
}

void
overlay_router_init(void)
{
	overlay_router_cache = kmem_cache_create("overlay_router",
	    sizeof (overlay_router_t), 0, overlay_router_cache_ctor,
	    overlay_router_cache_dtor, NULL, NULL, NULL, 0);
	overlay_net_cache = kmem_cache_create("overlay_router_net",
	    sizeof (overlay_net_t), 0, overlay_net_cache_ctor,
	    overlay_net_cache_dtor, NULL, NULL, NULL, 0);
	overlay_rtab_cache = kmem_cache_create("overlay_route_table",
	    sizeof (overlay_routetab_t), 0, overlay_rtab_cache_ctor,
	    overlay_rtab_cache_dtor, NULL, NULL, NULL, 0);
}

void
overlay_router_fini(void)
{
	kmem_cache_destroy(overlay_router_cache);
	kmem_cache_destroy(overlay_net_cache);
	kmem_cache_destroy(overlay_rtab_cache);
}
