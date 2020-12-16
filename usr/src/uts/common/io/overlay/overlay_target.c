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
 * Overlay device target cache management
 *
 * For more information, see the big theory statement in
 * uts/common/io/overlay/overlay.c
 */

#include <sys/types.h>
#include <sys/ethernet.h>
#include <sys/kmem.h>
#include <sys/policy.h>
#include <sys/sysmacros.h>
#include <sys/stream.h>
#include <sys/strsun.h>
#include <sys/strsubr.h>
#include <sys/mac_provider.h>
#include <sys/mac_client.h>
#include <sys/mac_client_priv.h>
#include <sys/vlan.h>
#include <sys/crc32.h>
#include <sys/cred.h>
#include <sys/file.h>
#include <sys/errno.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>

#include <inet/ip.h>
#include <inet/tcp.h>
#include <netinet/ip6.h>
#include <netinet/icmp6.h>
#include <netinet/udp.h>

#include <sys/overlay_impl.h>

/*
 * We normally should not need to do a msgpullup() on an mblk_t.
 * However, there are certain circumstances that shouldn't be common, but
 * still technically possible where we elect to use msgpullup() for our
 * own sanity (e.g. a header is split across more than one mblk_t).
 * Anything generating such awful mblk_ts is probably worth investigating,
 * so we define some dtrace probes to help identify those if they occur.
 */
#define	OVERLAY_PULLUPMSG(mp, reason) \
    DTRACE_PROBE2(__overlay_pullupmsg, mblk_t *, mp, const char *, reason)


/*
 * This is total straw man, but at least it's a prime number. Here we're
 * going to have to go through and do a lot of evaluation and understanding as
 * to how these target caches should grow and shrink, as well as, memory
 * pressure and evictions. This just gives us a starting point that'll be 'good
 * enough', until it's not.
 */
#define	OVERLAY_HSIZE	823

/*
 * We use this data structure to keep track of what requests have been actively
 * allocated to a given instance so we know what to put back on the pending
 * list.
 */
typedef struct overlay_target_hdl {
	minor_t oth_minor;		/* RO */
	zoneid_t oth_zoneid;		/* RO */
	int oth_oflags;			/* RO */
	list_node_t oth_link;		/* overlay_target_lock */
	kmutex_t oth_lock;
	list_t	oth_outstanding;	/* oth_lock */
} overlay_target_hdl_t;

typedef int (*overlay_target_copyin_f)(const void *, void **, size_t *, int);
typedef int (*overlay_target_ioctl_f)(overlay_target_hdl_t *, void *);
typedef int (*overlay_target_copyout_f)(void *, void *, size_t, int);

typedef struct overlay_target_ioctl {
	int		oti_cmd;	/* ioctl id */
	boolean_t	oti_write;	/* ioctl requires FWRITE */
	boolean_t	oti_ncopyout;	/* copyout data? */
	overlay_target_copyin_f oti_copyin;	/* copyin func */
	overlay_target_ioctl_f oti_func; /* function to call */
	overlay_target_copyout_f oti_copyout;	/* copyin func */
	size_t		oti_size;	/* size of user level structure */
} overlay_target_ioctl_t;

static kmem_cache_t *overlay_target_cache;
static kmem_cache_t *overlay_entry_cache;
static id_space_t *overlay_thdl_idspace;
static void *overlay_thdl_state;

/*
 * When we support overlay devices in the NGZ, then all of these need to become
 * zone aware, by plugging into the netstack engine and becoming per-netstack
 * data.
 */
static list_t overlay_thdl_list;
static kmutex_t overlay_target_lock;
static kcondvar_t overlay_target_condvar;
static list_t overlay_target_list;
static boolean_t overlay_target_excl;

/*
 * Outstanding data per hash table entry.
 */
static int overlay_ent_size = 128 * 1024;

/* ARGSUSED */
static int
overlay_target_cache_constructor(void *buf, void *arg, int kmflgs)
{
	overlay_target_t *ott = buf;

	mutex_init(&ott->ott_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&ott->ott_cond, NULL, CV_DRIVER, NULL);
	return (0);
}

/* ARGSUSED */
static void
overlay_target_cache_destructor(void *buf, void *arg)
{
	overlay_target_t *ott = buf;

	cv_destroy(&ott->ott_cond);
	mutex_destroy(&ott->ott_lock);
}

/* ARGSUSED */
static int
overlay_entry_cache_constructor(void *buf, void *arg, int kmflgs)
{
	overlay_target_entry_t *ote = buf;

	bzero(ote, sizeof (overlay_target_entry_t));
	mutex_init(&ote->ote_lock, NULL, MUTEX_DRIVER, NULL);
	return (0);
}

/* ARGSUSED */
static void
overlay_entry_cache_destructor(void *buf, void *arg)
{
	overlay_target_entry_t *ote = buf;

	mutex_destroy(&ote->ote_lock);
}

static uint64_t
overlay_mac_hash(const void *v)
{
	uint32_t crc;
	CRC32(crc, v, ETHERADDRL, -1U, crc32_table);
	return (crc);
}

static int
overlay_mac_cmp(const void *a, const void *b)
{
	return (bcmp(a, b, ETHERADDRL));
}

static uint64_t
overlay_ip_hash(const void *v)
{
	uint32_t crc;
	CRC32(crc, v, sizeof (struct in6_addr), -1U, crc32_table);
	return (crc);
}

static int
overlay_ip_cmp(const void *a, const void *b)
{
	return (bcmp(a, b, sizeof (struct in6_addr)));
}

static int
overlay_ip_avl(const void *a, const void *b)
{
	const overlay_target_entry_t *l = a;
	const overlay_target_entry_t *r = b;
	const struct in6_addr *l_ip = &l->ote_key.otk_ip;
	const struct in6_addr *r_ip = &r->ote_key.otk_ip;
	int i;

	ASSERT(l->ote_flags & OVERLAY_ENTRY_F_L3);
	ASSERT(r->ote_flags & OVERLAY_ENTRY_F_L3);

	for (i = 0; i < sizeof (struct in6_addr); i++) {
		if (l_ip->s6_addr[i] < r_ip->s6_addr[i])
			return (-1);
		if (l_ip->s6_addr[i] > r_ip->s6_addr[i])
			return (1);
	}
	return (0);
}

/* ARGSUSED */
static void
overlay_target_entry_dtor(void *arg)
{
	overlay_target_entry_t *ote = arg;

	ote->ote_flags = 0;
	bzero(&ote->ote_key, sizeof (ote->ote_key));
	bzero(&ote->ote_entry, sizeof (ote->ote_entry));
	ote->ote_ott = NULL;
	ote->ote_odd = NULL;
	freemsgchain(ote->ote_chead);
	ote->ote_chead = ote->ote_ctail = NULL;
	ote->ote_mbsize = 0;
	ote->ote_vtime = 0;
	kmem_cache_free(overlay_entry_cache, ote);
}

static int
overlay_mac_avl(const void *a, const void *b)
{
	const overlay_target_entry_t *l = a;
	const overlay_target_entry_t *r = b;
	const uint8_t *l_mac = l->ote_key.otk_mac;
	const uint8_t *r_mac = r->ote_key.otk_mac;
	int i;

	ASSERT0(l->ote_flags & OVERLAY_ENTRY_F_L3);
	ASSERT0(r->ote_flags & OVERLAY_ENTRY_F_L3);

	for (i = 0; i < ETHERADDRL; i++) {
		if (l_mac[i] > r_mac[i])
			return (1);
		else if (l_mac[i] < r_mac[i])
			return (-1);
	}

	return (0);
}

void
overlay_target_init(void)
{
	int ret;
	ret = ddi_soft_state_init(&overlay_thdl_state,
	    sizeof (overlay_target_hdl_t), 1);
	VERIFY(ret == 0);
	overlay_target_cache = kmem_cache_create("overlay_target",
	    sizeof (overlay_target_t), 0, overlay_target_cache_constructor,
	    overlay_target_cache_destructor, NULL, NULL, NULL, 0);
	overlay_entry_cache = kmem_cache_create("overlay_entry",
	    sizeof (overlay_target_entry_t), 0, overlay_entry_cache_constructor,
	    overlay_entry_cache_destructor, NULL, NULL, NULL, 0);
	mutex_init(&overlay_target_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&overlay_target_condvar, NULL, CV_DRIVER, NULL);
	list_create(&overlay_target_list, sizeof (overlay_target_entry_t),
	    offsetof(overlay_target_entry_t, ote_qlink));
	list_create(&overlay_thdl_list, sizeof (overlay_target_hdl_t),
	    offsetof(overlay_target_hdl_t, oth_link));
	overlay_thdl_idspace = id_space_create("overlay_target_minors",
	    OVERLAY_MINOR_START, INT32_MAX);
}

void
overlay_target_fini(void)
{
	id_space_destroy(overlay_thdl_idspace);
	list_destroy(&overlay_thdl_list);
	list_destroy(&overlay_target_list);
	cv_destroy(&overlay_target_condvar);
	mutex_destroy(&overlay_target_lock);
	kmem_cache_destroy(overlay_entry_cache);
	kmem_cache_destroy(overlay_target_cache);
	ddi_soft_state_fini(&overlay_thdl_state);
}

static void
overlay_cache_free(qqcache_t *qp, avl_tree_t *ap)
{
	overlay_target_entry_t *ote;

	/*
	 * Our AVL tree and hashtable contain the same elements,
	 * therefore we should just remove it from the tree, but then
	 * delete the entries when we remove them from the hash table
	 * (which happens through the qqcache dtor).
	 */
	while ((ote = avl_first(ap)) != NULL)
		avl_remove(ap, ote);

	avl_destroy(ap);
	for (ote = qqcache_first(qp); ote != NULL;
	    ote = qqcache_next(qp, ote)) {
		qqcache_remove(qp, ote);
	}
	qqcache_destroy(qp);
}

void
overlay_target_free(overlay_dev_t *odd)
{
	if (odd->odd_target == NULL)
		return;

	if (odd->odd_target->ott_mode == OVERLAY_TARGET_DYNAMIC) {
		overlay_cache_free(odd->odd_target->ott_u.ott_dyn.ott_cache,
		    &odd->odd_target->ott_u.ott_dyn.ott_tree);
		overlay_cache_free(odd->odd_target->ott_u.ott_dyn.ott_l3cache,
		    &odd->odd_target->ott_u.ott_dyn.ott_l3tree);
	}

	ASSERT(odd->odd_target->ott_ocount == 0);
	kmem_cache_free(overlay_target_cache, odd->odd_target);
}

int
overlay_target_busy()
{
	int ret;

	mutex_enter(&overlay_target_lock);
	ret = !list_is_empty(&overlay_thdl_list);
	mutex_exit(&overlay_target_lock);

	return (ret);
}

static void
overlay_target_queue(overlay_target_entry_t *entry)
{
	mutex_enter(&overlay_target_lock);
	mutex_enter(&entry->ote_ott->ott_lock);
	if (entry->ote_ott->ott_flags & OVERLAY_T_TEARDOWN) {
		mutex_exit(&entry->ote_ott->ott_lock);
		mutex_exit(&overlay_target_lock);
		return;
	}
	entry->ote_ott->ott_ocount++;
	mutex_exit(&entry->ote_ott->ott_lock);
	list_insert_tail(&overlay_target_list, entry);
	cv_signal(&overlay_target_condvar);
	mutex_exit(&overlay_target_lock);
}

void
overlay_target_quiesce(overlay_target_t *ott)
{
	if (ott == NULL)
		return;
	mutex_enter(&ott->ott_lock);
	ott->ott_flags |= OVERLAY_T_TEARDOWN;
	while (ott->ott_ocount != 0)
		cv_wait(&ott->ott_cond, &ott->ott_lock);
	mutex_exit(&ott->ott_lock);
}

/*
 * This functions assumes that the destination mode is OVERLAY_PLUGIN_D_IP |
 * OVERLAY_PLUGIN_D_PORT. As we don't have an implementation of anything else at
 * this time, say for NVGRE, we drop all packets that mcuh this.
 */
int
overlay_target_lookup(overlay_dev_t *odd, overlay_pkt_t *pkt, boolean_t is_l3,
    struct sockaddr *sock, socklen_t *slenp)
{
	int ret;
	struct sockaddr_in6 *v6;
	overlay_target_t *ott;
	overlay_target_entry_t *entry;
	qqcache_t *cache;
	avl_tree_t *avl;
	const void *key;

	ASSERT(odd->odd_target != NULL);

	/*
	 * At this point, the overlay device is in a mux which means that it's
	 * been activated. At this point, parts of the target, such as the mode
	 * and the destination are now read-only and we don't have to worry
	 * about synchronization for them.
	 */
	ott = odd->odd_target;
	if (ott->ott_dest != (OVERLAY_PLUGIN_D_IP | OVERLAY_PLUGIN_D_PORT))
		return (OVERLAY_TARGET_DROP);

	v6 = (struct sockaddr_in6 *)sock;
	bzero(v6, sizeof (struct sockaddr_in6));
	v6->sin6_family = AF_INET6;

	if (ott->ott_mode == OVERLAY_TARGET_POINT) {
		mutex_enter(&ott->ott_lock);
		bcopy(&ott->ott_u.ott_point.otp_ip, &v6->sin6_addr,
		    sizeof (struct in6_addr));
		v6->sin6_port = htons(ott->ott_u.ott_point.otp_port);
		mutex_exit(&ott->ott_lock);
		*slenp = sizeof (struct sockaddr_in6);

		return (OVERLAY_TARGET_OK);
	}

	ASSERT(ott->ott_mode == OVERLAY_TARGET_DYNAMIC);

	if (is_l3) {
		cache = ott->ott_u.ott_dyn.ott_l3cache;
		avl = &ott->ott_u.ott_dyn.ott_l3tree;
		key = &pkt->op_dstaddr;
	} else {
		cache = ott->ott_u.ott_dyn.ott_cache;
		avl = &ott->ott_u.ott_dyn.ott_tree;
		key = pkt->op_mhi.mhi_daddr;
	}

	v6 = (struct sockaddr_in6 *)sock;
	mutex_enter(&ott->ott_lock);
	entry = qqcache_lookup(cache, key);
	if (entry == NULL) {
		entry = kmem_cache_alloc(overlay_entry_cache,
		    KM_NOSLEEP | KM_NORMALPRI);
		if (entry == NULL) {
			mutex_exit(&ott->ott_lock);
			return (OVERLAY_TARGET_DROP);
		}
		if (is_l3) {
			entry->ote_flags |= OVERLAY_ENTRY_F_L3;
			bcopy(&pkt->op_dstaddr, &entry->ote_key.otk_ip,
			    sizeof (struct in6_addr));
		} else {
			bcopy(pkt->op_mhi.mhi_daddr, entry->ote_key.otk_mac,
			    ETHERADDRL);
		}
		entry->ote_chead = entry->ote_ctail = pkt->op_mblk;
		entry->ote_mbsize = msgsize(pkt->op_mblk);
		entry->ote_flags |= OVERLAY_ENTRY_F_PENDING;
		entry->ote_ott = ott;
		entry->ote_odd = odd;
		qqcache_insert(cache, entry);
		avl_add(avl, entry);
		mutex_exit(&ott->ott_lock);
		overlay_target_queue(entry);
		return (OVERLAY_TARGET_ASYNC);
	}
	qqcache_hold(cache, entry);
	mutex_exit(&ott->ott_lock);

	mutex_enter(&entry->ote_lock);
	if (entry->ote_flags & OVERLAY_ENTRY_F_DROP) {
		ret = OVERLAY_TARGET_DROP;
	} else if (entry->ote_flags & OVERLAY_ENTRY_F_VALID) {
		if (is_l3) {
			/*
			 * If we are routing this packet (is_l3 is B_TRUE),
			 * we've found the MAC for the VL3 dest IP (the guest's
			 * destination IP). Set the destination MAC for the
			 * packet to the guest's MAC and process it as a L2
			 * packet. The destination will adjust the vlan and
			 * source MAC as necessary before passing it to the
			 * upper vnic.
			 */

			/* drop const */
			void *destp = (void *)pkt->op_mhi.mhi_daddr;

			bcopy(entry->ote_entry.otp_mac, destp, ETHERADDRL);
			mutex_exit(&entry->ote_lock);
			ret = overlay_target_lookup(odd, pkt, B_FALSE, sock,
			    slenp);
			goto done;
		}

		bcopy(&entry->ote_entry.otp_ip, &v6->sin6_addr,
		    sizeof (struct in6_addr));
		v6->sin6_port = htons(entry->ote_entry.otp_port);
		*slenp = sizeof (struct sockaddr_in6);
		ret = OVERLAY_TARGET_OK;
	} else {
		size_t mlen = msgsize(pkt->op_mblk);

		if (mlen + entry->ote_mbsize > overlay_ent_size) {
			ret = OVERLAY_TARGET_DROP;
		} else {
			if (entry->ote_ctail != NULL) {
				ASSERT(entry->ote_ctail->b_next ==
				    NULL);
				entry->ote_ctail->b_next = pkt->op_mblk;
				entry->ote_ctail = pkt->op_mblk;
			} else {
				entry->ote_chead = pkt->op_mblk;
				entry->ote_ctail = pkt->op_mblk;
			}
			entry->ote_mbsize += mlen;
			if ((entry->ote_flags &
			    OVERLAY_ENTRY_F_PENDING) == 0) {
				entry->ote_flags |=
				    OVERLAY_ENTRY_F_PENDING;
				overlay_target_queue(entry);
			}
			ret = OVERLAY_TARGET_ASYNC;
		}
	}
	mutex_exit(&entry->ote_lock);

done:
	mutex_enter(&ott->ott_lock);
	qqcache_rele(cache, entry);
	mutex_exit(&ott->ott_lock);

	return (ret);
}

/* ARGSUSED */
static int
overlay_target_info(overlay_target_hdl_t *thdl, void *arg)
{
	overlay_dev_t *odd;
	overlay_targ_info_t *oti = arg;

	odd = overlay_hold_by_dlid(oti->oti_linkid);
	if (odd == NULL)
		return (ENOENT);

	mutex_enter(&odd->odd_lock);
	oti->oti_flags = 0;
	oti->oti_needs = odd->odd_plugin->ovp_dest;
	if (odd->odd_flags & OVERLAY_F_DEGRADED)
		oti->oti_flags |= OVERLAY_TARG_INFO_F_DEGRADED;
	if (odd->odd_flags & OVERLAY_F_ACTIVATED)
		oti->oti_flags |= OVERLAY_TARG_INFO_F_ACTIVE;
	oti->oti_vnetid = odd->odd_vid;
	mutex_exit(&odd->odd_lock);
	overlay_hold_rele(odd);
	return (0);
}

/* ARGSUSED */
static int
overlay_target_associate(overlay_target_hdl_t *thdl, void *arg)
{
	overlay_dev_t *odd;
	overlay_target_t *ott;
	overlay_router_t *orr;
	overlay_targ_associate_t *ota = arg;

	odd = overlay_hold_by_dlid(ota->ota_linkid);
	if (odd == NULL)
		return (ENOENT);

	if (ota->ota_id == 0) {
		overlay_hold_rele(odd);
		return (EINVAL);
	}

	if (ota->ota_mode != OVERLAY_TARGET_POINT &&
	    ota->ota_mode != OVERLAY_TARGET_DYNAMIC) {
		overlay_hold_rele(odd);
		return (EINVAL);
	}

	if (ota->ota_provides != odd->odd_plugin->ovp_dest) {
		overlay_hold_rele(odd);
		return (EINVAL);
	}

	if (ota->ota_mode == OVERLAY_TARGET_POINT) {
		if (ota->ota_provides & OVERLAY_PLUGIN_D_IP) {
			if (IN6_IS_ADDR_UNSPECIFIED(&ota->ota_point.otp_ip) ||
			    IN6_IS_ADDR_V4COMPAT(&ota->ota_point.otp_ip) ||
			    IN6_IS_ADDR_V4MAPPED_ANY(&ota->ota_point.otp_ip)) {
				overlay_hold_rele(odd);
				return (EINVAL);
			}
		}

		if (ota->ota_provides & OVERLAY_PLUGIN_D_PORT) {
			if (ota->ota_point.otp_port == 0) {
				overlay_hold_rele(odd);
				return (EINVAL);
			}
		}
	}

	ott = kmem_cache_alloc(overlay_target_cache, KM_SLEEP);
	ott->ott_flags = 0;
	ott->ott_ocount = 0;
	ott->ott_mode = ota->ota_mode;
	ott->ott_dest = ota->ota_provides;
	ott->ott_id = ota->ota_id;

	if (ott->ott_mode == OVERLAY_TARGET_POINT) {
		bcopy(&ota->ota_point, &ott->ott_u.ott_point,
		    sizeof (overlay_target_point_t));
		orr = NULL;
	} else {
		int ret;

		ret = qqcache_create(&ott->ott_u.ott_dyn.ott_cache,
		    odd->odd_cachesz, odd->odd_cachea, OVERLAY_HSIZE,
		    overlay_mac_hash, overlay_mac_cmp,
		    overlay_target_entry_dtor, sizeof (overlay_target_entry_t),
		    offsetof(overlay_target_entry_t, ote_cachelink),
		    offsetof(overlay_target_entry_t, ote_key.otk_mac),
		    KM_SLEEP);

		if (ret != 0) {
			kmem_cache_free(overlay_target_cache, ott);
			overlay_hold_rele(odd);
			return (ret);
		}

		/*
		 * At least initially, the L3 cache is the same size as
		 * the L2 cache. Perhaps later we'll add separate sizes.
		 */
		ret = qqcache_create(&ott->ott_u.ott_dyn.ott_l3cache,
		    odd->odd_cachesz, odd->odd_cachea, OVERLAY_HSIZE,
		    overlay_ip_hash, overlay_ip_cmp,
		    overlay_target_entry_dtor, sizeof (overlay_target_entry_t),
		    offsetof(overlay_target_entry_t, ote_cachelink),
		    offsetof(overlay_target_entry_t, ote_key.otk_ip),
		    KM_SLEEP);

		if (ret != 0) {
			qqcache_destroy(ott->ott_u.ott_dyn.ott_cache);
			kmem_cache_free(overlay_target_cache, ott);
			overlay_hold_rele(odd);
			return (ret);
		}

		avl_create(&ott->ott_u.ott_dyn.ott_tree, overlay_mac_avl,
		    sizeof (overlay_target_entry_t),
		    offsetof(overlay_target_entry_t, ote_avllink));

		avl_create(&ott->ott_u.ott_dyn.ott_l3tree, overlay_ip_avl,
		    sizeof (overlay_target_entry_t),
		    offsetof(overlay_target_entry_t, ote_avllink));

		orr = overlay_router_create(odd);
	}
	mutex_enter(&odd->odd_lock);
	if (odd->odd_flags & OVERLAY_F_VARPD) {
		mutex_exit(&odd->odd_lock);
		kmem_cache_free(overlay_target_cache, ott);
		overlay_router_free(orr);
		overlay_hold_rele(odd);
		return (EEXIST);
	}

	odd->odd_flags |= OVERLAY_F_VARPD;
	odd->odd_target = ott;
	odd->odd_router = orr;
	mutex_exit(&odd->odd_lock);

	overlay_hold_rele(odd);

	return (0);
}


/* ARGSUSED */
static int
overlay_target_degrade(overlay_target_hdl_t *thdl, void *arg)
{
	overlay_dev_t *odd;
	overlay_targ_degrade_t *otd = arg;

	odd = overlay_hold_by_dlid(otd->otd_linkid);
	if (odd == NULL)
		return (ENOENT);

	overlay_fm_degrade(odd, otd->otd_buf);
	overlay_hold_rele(odd);
	return (0);
}

/* ARGSUSED */
static int
overlay_target_restore(overlay_target_hdl_t *thdl, void *arg)
{
	overlay_dev_t *odd;
	overlay_targ_id_t *otid = arg;

	odd = overlay_hold_by_dlid(otid->otid_linkid);
	if (odd == NULL)
		return (ENOENT);

	overlay_fm_restore(odd);
	overlay_hold_rele(odd);
	return (0);
}

/* ARGSUSED */
static int
overlay_target_disassociate(overlay_target_hdl_t *thdl, void *arg)
{
	overlay_dev_t *odd;
	overlay_targ_id_t *otid = arg;

	odd = overlay_hold_by_dlid(otid->otid_linkid);
	if (odd == NULL)
		return (ENOENT);

	mutex_enter(&odd->odd_lock);
	odd->odd_flags &= ~OVERLAY_F_VARPD;
	mutex_exit(&odd->odd_lock);

	overlay_hold_rele(odd);
	return (0);

}

static int
overlay_target_lookup_request(overlay_target_hdl_t *thdl, void *arg)
{
	overlay_targ_lookup_t *otl = arg;
	overlay_target_entry_t *entry;
	clock_t ret, timeout;
	overlay_pkt_t pkt;
	const char *reason;

	timeout = ddi_get_lbolt() + drv_usectohz(MICROSEC);
again:
	mutex_enter(&overlay_target_lock);
	while (list_is_empty(&overlay_target_list)) {
		ret = cv_timedwait(&overlay_target_condvar,
		    &overlay_target_lock, timeout);
		if (ret == -1) {
			mutex_exit(&overlay_target_lock);
			return (ETIME);
		}
	}
	entry = list_remove_head(&overlay_target_list);
	mutex_exit(&overlay_target_lock);
	mutex_enter(&entry->ote_lock);
	if (entry->ote_flags & OVERLAY_ENTRY_F_VALID) {
		ASSERT(entry->ote_chead == NULL);
		mutex_exit(&entry->ote_lock);
		goto again;
	}
	ASSERT(entry->ote_chead != NULL);

	/*
	 * If we have a bogon that doesn't have a valid mac header, drop it and
	 * try again.
	 */
	if (overlay_pkt_init(&pkt, entry->ote_odd->odd_mh,
	    entry->ote_chead, &reason) != 0) {
		boolean_t queue = B_FALSE;
		mblk_t *mp = entry->ote_chead;

		entry->ote_chead = mp->b_next;
		mp->b_next = NULL;
		if (entry->ote_ctail == mp)
			entry->ote_ctail = entry->ote_chead;
		entry->ote_mbsize -= msgsize(mp);

		if (entry->ote_chead != NULL)
			queue = B_TRUE;
		mutex_exit(&entry->ote_lock);

		if (queue == B_TRUE)
			overlay_target_queue(entry);

		OVERLAY_FREEMSG(mp, reason);
		freemsg(mp);
		goto again;
	}

	otl->otl_dlid = entry->ote_odd->odd_linkid;
	otl->otl_reqid = (uintptr_t)entry;
	otl->otl_varpdid = entry->ote_ott->ott_id;
	otl->otl_vnetid = entry->ote_odd->odd_vid;
	otl->otl_hdrsize = pkt.op_mhi.mhi_hdrsize;
	otl->otl_pktsize = msgsize(entry->ote_chead) - otl->otl_hdrsize;

	if (OTE_IS_L3(entry)) {
		otl->otl_l3lookup = B_TRUE;
		bcopy(&pkt.op_dstaddr, &otl->otl_u.otl_l3.otl3_dest,
		    sizeof (struct in6_addr));

		DTRACE_PROBE3(__overlay_vl3__lookup__req,
		    uintptr_t, otl->otl_reqid, uint64_t, otl->otl_vnetid,
		    struct in6_addr *, &otl->otl_u.otl_l3.otl3_dest);
	} else {
		otl->otl_l3lookup = B_FALSE;
		bcopy(pkt.op_mhi.mhi_daddr, otl->otl_u.otl_l2.otl2_dstaddr,
		    ETHERADDRL);
		bcopy(pkt.op_mhi.mhi_saddr, otl->otl_u.otl_l2.otl2_srcaddr,
		    ETHERADDRL);
		otl->otl_u.otl_l2.otl2_dsttype = pkt.op_mhi.mhi_dsttype;
		otl->otl_u.otl_l2.otl2_sap = pkt.op_mhi.mhi_bindsap;
		otl->otl_u.otl_l2.otl2_vlan = VLAN_ID(pkt.op_mhi.mhi_tci);

		DTRACE_PROBE3(__overlay_vl2__lookup__req,
		    uintptr_t, otl->otl_reqid, uint64_t, otl->otl_vnetid,
		    uint8_t *, otl->otl_u.otl_l2.otl2_dstaddr);
	}
	mutex_exit(&entry->ote_lock);

	mutex_enter(&thdl->oth_lock);
	list_insert_tail(&thdl->oth_outstanding, entry);
	mutex_exit(&thdl->oth_lock);

	return (0);
}

static int
overlay_target_lookup_respond(overlay_target_hdl_t *thdl, void *arg)
{
	const overlay_targ_resp_t *otr = arg;
	overlay_target_entry_t *entry;
	mblk_t *mp;

	mutex_enter(&thdl->oth_lock);
	for (entry = list_head(&thdl->oth_outstanding); entry != NULL;
	    entry = list_next(&thdl->oth_outstanding, entry)) {
		if ((uintptr_t)entry == otr->otr_reqid)
			break;
	}

	if (entry == NULL) {
		mutex_exit(&thdl->oth_lock);
		return (EINVAL);
	}
	list_remove(&thdl->oth_outstanding, entry);
	mutex_exit(&thdl->oth_lock);

	/*
	 * For L3 lookups, we depend on varpd injecting the L2
	 * entry prior to issuing the reply ioctl to an L3 request,
	 * so once we have the L3 entry, we should be able to
	 * process any L3 queued messages.
	 */
	mutex_enter(&entry->ote_lock);
	bcopy(&otr->otr_answer, &entry->ote_entry,
	    sizeof (overlay_target_point_t));
	entry->ote_flags &= ~OVERLAY_ENTRY_F_PENDING;
	entry->ote_flags |= OVERLAY_ENTRY_F_VALID;
	mp = entry->ote_chead;
	entry->ote_chead = NULL;
	entry->ote_ctail = NULL;
	entry->ote_mbsize = 0;
	entry->ote_vtime = gethrtime();

	if (OTE_IS_L3(entry)) {
		DTRACE_PROBE2(__overlay_vl3__lookup__resp,
		    uintptr_t, otr->otr_reqid,
		    uint8_t *, entry->ote_entry.otp_mac);
	} else {
		uint16_t port = ntohs(entry->ote_entry.otp_port);

		DTRACE_PROBE3(__overlay_vl2__lookup__resp,
		    uintptr_t, otr->otr_reqid,
		    struct in6_addr *, &entry->ote_entry.otp_ip,
		    uint16_t, port);
	}
	mutex_exit(&entry->ote_lock);

	/*
	 * For now do an in-situ drain.
	 */
	mp = overlay_m_tx(entry->ote_odd, mp);
	freemsgchain(mp);

	mutex_enter(&entry->ote_ott->ott_lock);
	entry->ote_ott->ott_ocount--;
	cv_signal(&entry->ote_ott->ott_cond);
	mutex_exit(&entry->ote_ott->ott_lock);

	return (0);
}

static int
overlay_target_lookup_drop(overlay_target_hdl_t *thdl, void *arg)
{
	const overlay_targ_resp_t *otr = arg;
	overlay_target_entry_t *entry;
	mblk_t *mp;
	boolean_t queue = B_FALSE;

	mutex_enter(&thdl->oth_lock);
	for (entry = list_head(&thdl->oth_outstanding); entry != NULL;
	    entry = list_next(&thdl->oth_outstanding, entry)) {
		if ((uintptr_t)entry == otr->otr_reqid)
			break;
	}

	if (entry == NULL) {
		mutex_exit(&thdl->oth_lock);
		return (EINVAL);
	}
	list_remove(&thdl->oth_outstanding, entry);
	mutex_exit(&thdl->oth_lock);

	mutex_enter(&entry->ote_lock);

	/* Safeguard against a confused varpd */
	if (entry->ote_flags & OVERLAY_ENTRY_F_VALID) {
		entry->ote_flags &= ~OVERLAY_ENTRY_F_PENDING;
		DTRACE_PROBE1(overlay__target__valid__drop,
		    overlay_target_entry_t *, entry);
		mutex_exit(&entry->ote_lock);
		goto done;
	}

	mp = entry->ote_chead;
	if (mp != NULL) {
		entry->ote_chead = mp->b_next;
		mp->b_next = NULL;
		if (entry->ote_ctail == mp)
			entry->ote_ctail = entry->ote_chead;
		entry->ote_mbsize -= msgsize(mp);
	}
	if (entry->ote_chead != NULL) {
		queue = B_TRUE;
		entry->ote_flags |= OVERLAY_ENTRY_F_PENDING;
	} else {
		entry->ote_flags &= ~OVERLAY_ENTRY_F_PENDING;
	}
	mutex_exit(&entry->ote_lock);

	if (queue == B_TRUE)
		overlay_target_queue(entry);

	OVERLAY_FREEMSG(mp, "received OVERLAY_TARGET_DROP ioctl");
	freemsg(mp);

done:
	mutex_enter(&entry->ote_ott->ott_lock);
	entry->ote_ott->ott_ocount--;
	cv_signal(&entry->ote_ott->ott_cond);
	mutex_exit(&entry->ote_ott->ott_lock);

	return (0);
}

/* ARGSUSED */
static int
overlay_target_pkt_copyin(const void *ubuf, void **outp, size_t *bsize,
    int flags)
{
	overlay_targ_pkt_t *pkt;
	overlay_targ_pkt32_t *pkt32;

	pkt = kmem_alloc(sizeof (overlay_targ_pkt_t), KM_SLEEP);
	*outp = pkt;
	*bsize = sizeof (overlay_targ_pkt_t);
	if (ddi_model_convert_from(flags & FMODELS) == DDI_MODEL_ILP32) {
		uintptr_t addr;

		if (ddi_copyin(ubuf, pkt, sizeof (overlay_targ_pkt32_t),
		    flags & FKIOCTL) != 0) {
			kmem_free(pkt, *bsize);
			return (EFAULT);
		}
		pkt32 = (overlay_targ_pkt32_t *)pkt;
		addr = pkt32->otp_buf;
		pkt->otp_buf = (void *)addr;
	} else {
		if (ddi_copyin(ubuf, pkt, *bsize, flags & FKIOCTL) != 0) {
			kmem_free(pkt, *bsize);
			return (EFAULT);
		}
	}
	return (0);
}

static int
overlay_target_pkt_copyout(void *ubuf, void *buf, size_t bufsize,
    int flags)
{
	if (ddi_model_convert_from(flags & FMODELS) == DDI_MODEL_ILP32) {
		overlay_targ_pkt_t *pkt = buf;
		overlay_targ_pkt32_t *pkt32 = buf;
		uintptr_t addr = (uintptr_t)pkt->otp_buf;
		pkt32->otp_buf = (caddr32_t)addr;
		if (ddi_copyout(buf, ubuf, sizeof (overlay_targ_pkt32_t),
		    flags & FKIOCTL) != 0)
			return (EFAULT);
	} else {
		if (ddi_copyout(buf, ubuf, bufsize, flags & FKIOCTL) != 0)
			return (EFAULT);
	}
	return (0);
}

static int
overlay_target_packet(overlay_target_hdl_t *thdl, void *arg)
{
	overlay_targ_pkt_t *pkt = arg;
	overlay_target_entry_t *entry;
	mblk_t *mp;
	size_t mlen;
	size_t boff;

	mutex_enter(&thdl->oth_lock);
	for (entry = list_head(&thdl->oth_outstanding); entry != NULL;
	    entry = list_next(&thdl->oth_outstanding, entry)) {
		if ((uintptr_t)entry == pkt->otp_reqid)
			break;
	}

	if (entry == NULL) {
		mutex_exit(&thdl->oth_lock);
		return (EINVAL);
	}
	mutex_enter(&entry->ote_lock);
	mutex_exit(&thdl->oth_lock);
	mp = entry->ote_chead;
	/* Protect against a rogue varpd */
	if (mp == NULL) {
		mutex_exit(&entry->ote_lock);
		return (EINVAL);
	}
	mlen = MIN(msgsize(mp), pkt->otp_size);
	pkt->otp_size = mlen;
	boff = 0;
	while (mlen > 0) {
		size_t wlen = MIN(MBLKL(mp), mlen);
		if (ddi_copyout(mp->b_rptr,
		    (void *)((uintptr_t)pkt->otp_buf + boff),
		    wlen, 0) != 0) {
			mutex_exit(&entry->ote_lock);
			return (EFAULT);
		}
		mlen -= wlen;
		boff += wlen;
		mp = mp->b_cont;
	}
	mutex_exit(&entry->ote_lock);
	return (0);
}

static int
overlay_target_inject(overlay_target_hdl_t *thdl, void *arg)
{
	overlay_targ_pkt_t *pkt = arg;
	overlay_target_entry_t *entry;
	overlay_dev_t *odd;
	mblk_t *mp;

	if (pkt->otp_size > ETHERMAX + VLAN_TAGSZ)
		return (EINVAL);

	mp = allocb(pkt->otp_size, 0);
	if (mp == NULL)
		return (ENOMEM);

	if (ddi_copyin(pkt->otp_buf, mp->b_rptr, pkt->otp_size, 0) != 0) {
		freeb(mp);
		return (EFAULT);
	}
	mp->b_wptr += pkt->otp_size;

	if (pkt->otp_linkid != UINT64_MAX) {
		odd = overlay_hold_by_dlid(pkt->otp_linkid);
		if (odd == NULL) {
			freeb(mp);
			return (ENOENT);
		}
	} else {
		mutex_enter(&thdl->oth_lock);
		for (entry = list_head(&thdl->oth_outstanding); entry != NULL;
		    entry = list_next(&thdl->oth_outstanding, entry)) {
			if ((uintptr_t)entry == pkt->otp_reqid)
				break;
		}

		if (entry == NULL) {
			mutex_exit(&thdl->oth_lock);
			freeb(mp);
			return (ENOENT);
		}
		odd = entry->ote_odd;
		mutex_exit(&thdl->oth_lock);
	}

	mutex_enter(&odd->odd_lock);
	overlay_io_start(odd, OVERLAY_F_IN_RX);
	mutex_exit(&odd->odd_lock);

	mac_rx(odd->odd_mh, NULL, mp);

	mutex_enter(&odd->odd_lock);
	overlay_io_done(odd, OVERLAY_F_IN_RX);
	mutex_exit(&odd->odd_lock);

	return (0);
}

static int
overlay_target_resend(overlay_target_hdl_t *thdl, void *arg)
{
	overlay_targ_pkt_t *pkt = arg;
	overlay_target_entry_t *entry;
	overlay_dev_t *odd;
	mblk_t *mp;

	if (pkt->otp_size > ETHERMAX + VLAN_TAGSZ)
		return (EINVAL);

	mp = allocb(pkt->otp_size, 0);
	if (mp == NULL)
		return (ENOMEM);

	if (ddi_copyin(pkt->otp_buf, mp->b_rptr, pkt->otp_size, 0) != 0) {
		freeb(mp);
		return (EFAULT);
	}
	mp->b_wptr += pkt->otp_size;

	if (pkt->otp_linkid != UINT64_MAX) {
		odd = overlay_hold_by_dlid(pkt->otp_linkid);
		if (odd == NULL) {
			freeb(mp);
			return (ENOENT);
		}
	} else {
		mutex_enter(&thdl->oth_lock);
		for (entry = list_head(&thdl->oth_outstanding); entry != NULL;
		    entry = list_next(&thdl->oth_outstanding, entry)) {
			if ((uintptr_t)entry == pkt->otp_reqid)
				break;
		}

		if (entry == NULL) {
			mutex_exit(&thdl->oth_lock);
			freeb(mp);
			return (ENOENT);
		}
		odd = entry->ote_odd;
		mutex_exit(&thdl->oth_lock);
	}

	mp = overlay_m_tx(odd, mp);
	freemsgchain(mp);

	return (0);
}

typedef struct overlay_targ_list_int {
	boolean_t	otli_count;
	uint32_t	otli_cur;
	uint32_t	otli_nents;
	uint32_t	otli_ents[];
} overlay_targ_list_int_t;

static int
overlay_target_list_copyin(const void *ubuf, void **outp, size_t *bsize,
    int flags)
{
	overlay_targ_list_t n;
	overlay_targ_list_int_t *otl;

	if (ddi_copyin(ubuf, &n, sizeof (overlay_targ_list_t),
	    flags & FKIOCTL) != 0)
		return (EFAULT);

	/*
	 */
	if (n.otl_nents >= INT32_MAX / sizeof (uint32_t))
		return (EINVAL);
	*bsize = sizeof (overlay_targ_list_int_t) +
	    sizeof (uint32_t) * n.otl_nents;
	otl = kmem_zalloc(*bsize, KM_SLEEP);
	otl->otli_cur = 0;
	otl->otli_nents = n.otl_nents;
	if (otl->otli_nents != 0) {
		otl->otli_count = B_FALSE;
		if (ddi_copyin((void *)((uintptr_t)ubuf +
		    offsetof(overlay_targ_list_t, otl_ents)),
		    otl->otli_ents, n.otl_nents * sizeof (uint32_t),
		    flags & FKIOCTL) != 0) {
			kmem_free(otl, *bsize);
			return (EFAULT);
		}
	} else {
		otl->otli_count = B_TRUE;
	}

	*outp = otl;
	return (0);
}

static int
overlay_target_ioctl_list_cb(overlay_dev_t *odd, void *arg)
{
	overlay_targ_list_int_t *otl = arg;

	if (otl->otli_cur < otl->otli_nents)
		otl->otli_ents[otl->otli_cur] = odd->odd_linkid;
	otl->otli_cur++;
	return (0);
}

/* ARGSUSED */
static int
overlay_target_ioctl_list(overlay_target_hdl_t *thdl, void *arg)
{
	overlay_dev_iter(overlay_target_ioctl_list_cb, arg);
	return (0);
}

/* ARGSUSED */
static int
overlay_target_list_copyout(void *ubuf, void *buf, size_t bufsize, int flags)
{
	overlay_targ_list_int_t *otl = buf;

	if (ddi_copyout(&otl->otli_cur, ubuf, sizeof (uint32_t),
	    flags & FKIOCTL) != 0)
		return (EFAULT);

	if (otl->otli_count == B_FALSE) {
		if (ddi_copyout(otl->otli_ents,
		    (void *)((uintptr_t)ubuf +
		    offsetof(overlay_targ_list_t, otl_ents)),
		    sizeof (uint32_t) * otl->otli_nents,
		    flags & FKIOCTL) != 0)
			return (EFAULT);
	}
	return (0);
}

/* ARGSUSED */
static int
overlay_target_cache_get(overlay_target_hdl_t *thdl, void *arg)
{
	int ret = 0;
	overlay_dev_t *odd;
	overlay_target_t *ott;
	overlay_targ_cache_t *otc = arg;

	odd = overlay_hold_by_dlid(otc->otc_linkid);
	if (odd == NULL)
		return (ENOENT);

	mutex_enter(&odd->odd_lock);
	if (!(odd->odd_flags & OVERLAY_F_VARPD)) {
		mutex_exit(&odd->odd_lock);
		overlay_hold_rele(odd);
		return (ENXIO);
	}
	ott = odd->odd_target;
	if (ott->ott_mode != OVERLAY_TARGET_POINT &&
	    ott->ott_mode != OVERLAY_TARGET_DYNAMIC) {
		mutex_exit(&odd->odd_lock);
		overlay_hold_rele(odd);
		return (ENOTSUP);
	}
	mutex_enter(&ott->ott_lock);
	mutex_exit(&odd->odd_lock);

	if (ott->ott_mode == OVERLAY_TARGET_POINT) {
		otc->otc_entry.otce_flags = 0;
		bcopy(&ott->ott_u.ott_point, &otc->otc_entry.otce_dest,
		    sizeof (overlay_target_point_t));
	} else {
		qqcache_t *cache;
		overlay_target_entry_t *ote;
		const void *key;

		if (OTCE_IS_L3(&otc->otc_entry)) {
			cache = ott->ott_u.ott_dyn.ott_l3cache;
			key = &otc->otc_entry.otce_dest.otp_ip;
		} else {
			cache = ott->ott_u.ott_dyn.ott_cache;
			key = otc->otc_entry.otce_mac;
		}

		ote = qqcache_lookup(cache, key);

		if (ote == NULL) {
			ret = ENOENT;
			goto done;
		}

		mutex_enter(&ote->ote_lock);

		if (OTCE_IS_L3(&otc->otc_entry))
			VERIFY(OTE_IS_L3(ote));
		else
			VERIFY(!OTE_IS_L3(ote));

		if ((ote->ote_flags & OVERLAY_ENTRY_F_VALID_MASK) == 0) {
			mutex_exit(&ote->ote_lock);
			ret = ENOENT;
			goto done;
		}

		/* Set the flags early in case the entry is marked 'drop' */
		otc->otc_entry.otce_flags = 0;
		if (OTE_IS_L3(ote))
			otc->otc_entry.otce_flags |= OVERLAY_TARGET_CACHE_L3;

		if (ote->ote_flags & OVERLAY_ENTRY_F_DROP) {
			mutex_exit(&ote->ote_lock);
			otc->otc_entry.otce_flags |= OVERLAY_TARGET_CACHE_DROP;
			goto done;
		}

		bcopy(&ote->ote_entry, &otc->otc_entry.otce_dest,
		    sizeof (overlay_target_point_t));
		mutex_exit(&ote->ote_lock);
	}

done:
	mutex_exit(&ott->ott_lock);
	overlay_hold_rele(odd);

	return (ret);
}

/* ARGSUSED */
static int
overlay_target_cache_set(overlay_target_hdl_t *thdl, void *arg)
{
	overlay_dev_t *odd;
	overlay_target_t *ott;
	overlay_target_entry_t *ote;
	overlay_targ_cache_t *otc = arg;
	mblk_t *mp = NULL;

	if (otc->otc_entry.otce_flags & ~OVERLAY_TARGET_CACHE_DROP)
		return (EINVAL);

	odd = overlay_hold_by_dlid(otc->otc_linkid);
	if (odd == NULL)
		return (ENOENT);

	mutex_enter(&odd->odd_lock);
	if (!(odd->odd_flags & OVERLAY_F_VARPD)) {
		mutex_exit(&odd->odd_lock);
		overlay_hold_rele(odd);
		return (ENXIO);
	}
	ott = odd->odd_target;
	if (ott->ott_mode != OVERLAY_TARGET_DYNAMIC) {
		mutex_exit(&odd->odd_lock);
		overlay_hold_rele(odd);
		return (ENOTSUP);
	}
	mutex_enter(&ott->ott_lock);
	mutex_exit(&odd->odd_lock);

	ote = qqcache_lookup(ott->ott_u.ott_dyn.ott_cache,
	    otc->otc_entry.otce_mac);
	if (ote == NULL) {
		ote = kmem_cache_alloc(overlay_entry_cache, KM_SLEEP);
		if (OTCE_IS_L3(&otc->otc_entry)) {
			bcopy(&otc->otc_entry.otce_dest.otp_ip,
			    &ote->ote_key.otk_ip, sizeof (struct in6_addr));
		} else {
			bcopy(otc->otc_entry.otce_mac, ote->ote_key.otk_mac,
			    ETHERADDRL);
		}
		ote->ote_chead = ote->ote_ctail = NULL;
		ote->ote_mbsize = 0;
		ote->ote_ott = ott;
		ote->ote_odd = odd;
		mutex_enter(&ote->ote_lock);
		qqcache_insert(ott->ott_u.ott_dyn.ott_cache, ote);
		avl_add(&ott->ott_u.ott_dyn.ott_tree, ote);
	} else {
		mutex_enter(&ote->ote_lock);
	}

	if (otc->otc_entry.otce_flags & OVERLAY_TARGET_CACHE_DROP) {
		ote->ote_flags |= OVERLAY_ENTRY_F_DROP;
	} else {
		ote->ote_flags |= OVERLAY_ENTRY_F_VALID;
		bcopy(&otc->otc_entry.otce_dest, &ote->ote_entry,
		    sizeof (overlay_target_point_t));
		mp = ote->ote_chead;
		ote->ote_chead = NULL;
		ote->ote_ctail = NULL;
		ote->ote_mbsize = 0;
		ote->ote_vtime = gethrtime();
	}

	mutex_exit(&ote->ote_lock);
	mutex_exit(&ott->ott_lock);

	if (mp != NULL) {
		mp = overlay_m_tx(ote->ote_odd, mp);
		freemsgchain(mp);
	}

	overlay_hold_rele(odd);

	return (0);
}

/* ARGSUSED */
static int
overlay_target_cache_remove(overlay_target_hdl_t *thdl, void *arg)
{
	int ret = 0;
	overlay_dev_t *odd;
	overlay_target_t *ott;
	overlay_target_entry_t *ote;
	overlay_targ_cache_t *otc = arg;
	qqcache_t *cache;
	const void *key;

	odd = overlay_hold_by_dlid(otc->otc_linkid);
	if (odd == NULL)
		return (ENOENT);

	mutex_enter(&odd->odd_lock);
	if (!(odd->odd_flags & OVERLAY_F_VARPD)) {
		mutex_exit(&odd->odd_lock);
		overlay_hold_rele(odd);
		return (ENXIO);
	}
	ott = odd->odd_target;
	if (ott->ott_mode != OVERLAY_TARGET_DYNAMIC) {
		mutex_exit(&odd->odd_lock);
		overlay_hold_rele(odd);
		return (ENOTSUP);
	}
	mutex_enter(&ott->ott_lock);
	mutex_exit(&odd->odd_lock);

	if (OTCE_IS_L3(&otc->otc_entry)) {
		cache = ott->ott_u.ott_dyn.ott_l3cache;
		key = &otc->otc_entry.otce_dest.otp_ip;
	} else {
		cache = ott->ott_u.ott_dyn.ott_cache;
		key = otc->otc_entry.otce_mac;
	}

	ote = qqcache_lookup(cache, key);
	if (ote != NULL) {
		mutex_enter(&ote->ote_lock);
		ote->ote_flags &= ~OVERLAY_ENTRY_F_VALID_MASK;
		mutex_exit(&ote->ote_lock);
		ret = 0;
	} else {
		ret = ENOENT;
	}

	mutex_exit(&ott->ott_lock);
	overlay_hold_rele(odd);

	return (ret);
}

static void
overlay_target_cache_flush_avl(avl_tree_t *avl)
{
	overlay_target_entry_t *ote;

	for (ote = avl_first(avl); ote != NULL; ote = AVL_NEXT(avl, ote)) {
		mutex_enter(&ote->ote_lock);
		ote->ote_flags &= ~OVERLAY_ENTRY_F_VALID_MASK;
		mutex_exit(&ote->ote_lock);
	}
}

/* ARGSUSED */
static int
overlay_target_cache_flush(overlay_target_hdl_t *thdl, void *arg)
{
	overlay_dev_t *odd;
	overlay_target_t *ott;
	overlay_targ_cache_t *otc = arg;

	odd = overlay_hold_by_dlid(otc->otc_linkid);
	if (odd == NULL)
		return (ENOENT);

	mutex_enter(&odd->odd_lock);
	if (!(odd->odd_flags & OVERLAY_F_VARPD)) {
		mutex_exit(&odd->odd_lock);
		overlay_hold_rele(odd);
		return (ENXIO);
	}
	ott = odd->odd_target;
	if (ott->ott_mode != OVERLAY_TARGET_DYNAMIC) {
		mutex_exit(&odd->odd_lock);
		overlay_hold_rele(odd);
		return (ENOTSUP);
	}
	mutex_enter(&ott->ott_lock);
	mutex_exit(&odd->odd_lock);

	overlay_target_cache_flush_avl(&ott->ott_u.ott_dyn.ott_tree);
	overlay_target_cache_flush_avl(&ott->ott_u.ott_dyn.ott_l3tree);

	mutex_exit(&ott->ott_lock);
	overlay_hold_rele(odd);

	return (0);
}

static int
overlay_target_cache_iter_copyin(const void *ubuf, void **outp, size_t *bsize,
    int flags)
{
	overlay_targ_cache_iter_t base, *iter;

	if (ddi_copyin(ubuf, &base, sizeof (overlay_targ_cache_iter_t),
	    flags & FKIOCTL) != 0)
		return (EFAULT);

	if (base.otci_count > OVERLAY_TARGET_ITER_MAX)
		return (E2BIG);

	if (base.otci_count == 0)
		return (EINVAL);

	*bsize = sizeof (overlay_targ_cache_iter_t) +
	    base.otci_count * sizeof (overlay_targ_cache_entry_t);
	iter = kmem_alloc(*bsize, KM_SLEEP);
	bcopy(&base, iter, sizeof (overlay_targ_cache_iter_t));
	*outp = iter;

	return (0);
}

typedef struct overlay_targ_cache_marker {
	overlay_target_key_t	otcm_key;
	uint8_t			otcm_done;
	uint8_t			otcm_l3;
} overlay_targ_cache_marker_t;

/* cstyle can't decide if this should be 4 spaces or 1 tab */
/* BEGIN CSTYLED */
CTASSERT(sizeof (overlay_targ_cache_marker_t) ==
    (OVERLAY_TARG_NMARKER * sizeof (uint32_t)));
/* END CSTYLED */

/* ARGSUSED */
static int
overlay_target_cache_iter(overlay_target_hdl_t *thdl, void *arg)
{
	overlay_dev_t *odd;
	overlay_target_t *ott;
	overlay_target_entry_t lookup, *ent;
	overlay_targ_cache_marker_t *mark;
	avl_index_t where;
	avl_tree_t *avl;
	uint16_t written = 0;

	overlay_targ_cache_iter_t *iter = arg;
	mark = (void *)&iter->otci_marker;

	if (mark->otcm_done != 0) {
		iter->otci_count = 0;
		return (0);
	}

	odd = overlay_hold_by_dlid(iter->otci_linkid);
	if (odd == NULL)
		return (ENOENT);

	mutex_enter(&odd->odd_lock);
	if (!(odd->odd_flags & OVERLAY_F_VARPD)) {
		mutex_exit(&odd->odd_lock);
		overlay_hold_rele(odd);
		return (ENXIO);
	}
	ott = odd->odd_target;
	if (ott->ott_mode != OVERLAY_TARGET_DYNAMIC &&
	    ott->ott_mode != OVERLAY_TARGET_POINT) {
		mutex_exit(&odd->odd_lock);
		overlay_hold_rele(odd);
		return (ENOTSUP);
	}

	/*
	 * Holding this lock across the entire iteration probably isn't very
	 * good. We should perhaps add an r/w lock for the avl tree. But we'll
	 * wait until we now it's necessary before we do more.
	 */
	mutex_enter(&ott->ott_lock);
	mutex_exit(&odd->odd_lock);

	if (ott->ott_mode == OVERLAY_TARGET_POINT) {
		overlay_targ_cache_entry_t *out = &iter->otci_ents[0];
		bzero(out->otce_mac, ETHERADDRL);
		out->otce_flags = 0;
		bcopy(&ott->ott_u.ott_point, &out->otce_dest,
		    sizeof (overlay_target_point_t));
		written++;
		mark->otcm_done = 1;
	}

again:
	if (mark->otcm_l3) {
		avl = &ott->ott_u.ott_dyn.ott_l3tree;
		bcopy(&mark->otcm_key.otk_ip, &lookup.ote_key.otk_ip,
		    sizeof (struct in6_addr));
	} else {
		avl = &ott->ott_u.ott_dyn.ott_tree;
		bcopy(mark->otcm_key.otk_mac, lookup.ote_key.otk_mac,
		    ETHERADDRL);
	}
	ent = avl_find(avl, &lookup, &where);

	/*
	 * NULL ent means that the entry does not exist, so we want to start
	 * with the closest node in the tree. This means that we implicitly rely
	 * on the tree's order and the first node will be the mac 00:00:00:00:00
	 * and the last will be ff:ff:ff:ff:ff:ff. For the L3 tree, we
	 * similarly rely on the first node would be ::0 and the last node
	 * would be ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff.
	 */
	if (ent == NULL) {
		ent = avl_nearest(avl, where, AVL_AFTER);
		if (ent == NULL) {
			if (!mark->otcm_l3) {
				mark->otcm_l3 = 1;
				goto again;
			}
			mark->otcm_done = 1;
			goto done;
		}
	}

	for (; ent != NULL && written < iter->otci_count;
	    ent = AVL_NEXT(avl, ent)) {
		overlay_targ_cache_entry_t *out = &iter->otci_ents[written];
		mutex_enter(&ent->ote_lock);
		if ((ent->ote_flags & OVERLAY_ENTRY_F_VALID_MASK) == 0) {
			mutex_exit(&ent->ote_lock);
			continue;
		}
		out->otce_flags = 0;
		if (OTE_IS_L3(ent)) {
			out->otce_flags |= OVERLAY_TARGET_CACHE_L3;
			bcopy(&ent->ote_key.otk_ip, &out->otce_dest.otp_ip,
			    sizeof (struct in6_addr));
		} else {
			bcopy(ent->ote_key.otk_mac, out->otce_mac, ETHERADDRL);
		}
		if (ent->ote_flags & OVERLAY_ENTRY_F_DROP)
			out->otce_flags |= OVERLAY_TARGET_CACHE_DROP;
		if (ent->ote_flags & OVERLAY_ENTRY_F_VALID) {
			bcopy(&ent->ote_entry, &out->otce_dest,
			    sizeof (overlay_target_point_t));
		}
		written++;
		mutex_exit(&ent->ote_lock);
	}

	if (ent != NULL) {
		if (OTE_IS_L3(ent)) {
			bcopy(&ent->ote_key.otk_ip, &mark->otcm_key.otk_ip,
			    sizeof (struct in6_addr));
		} else {
			bcopy(ent->ote_key.otk_mac, mark->otcm_key.otk_mac,
			    ETHERADDRL);
		}
	} else {
		if (!mark->otcm_l3) {
			mark->otcm_l3 = 1;
			goto again;
		}
		mark->otcm_done = 1;
	}

done:
	iter->otci_count = written;
	mutex_exit(&ott->ott_lock);
	overlay_hold_rele(odd);

	return (0);
}

/* ARGSUSED */
static int
overlay_target_cache_iter_copyout(void *ubuf, void *buf, size_t bufsize,
    int flags)
{
	size_t outsize;
	const overlay_targ_cache_iter_t *iter = buf;

	outsize = sizeof (overlay_targ_cache_iter_t) +
	    iter->otci_count * sizeof (overlay_targ_cache_entry_t);

	if (ddi_copyout(buf, ubuf, outsize, flags & FKIOCTL) != 0)
		return (EFAULT);

	return (0);
}

static overlay_target_ioctl_t overlay_target_ioctab[] = {
	{ OVERLAY_TARG_INFO, B_TRUE, B_TRUE,
		NULL, overlay_target_info,
		NULL, sizeof (overlay_targ_info_t)	},
	{ OVERLAY_TARG_ASSOCIATE, B_TRUE, B_FALSE,
		NULL, overlay_target_associate,
		NULL, sizeof (overlay_targ_associate_t)	},
	{ OVERLAY_TARG_DISASSOCIATE, B_TRUE, B_FALSE,
		NULL, overlay_target_disassociate,
		NULL, sizeof (overlay_targ_id_t)	},
	{ OVERLAY_TARG_DEGRADE, B_TRUE, B_FALSE,
		NULL, overlay_target_degrade,
		NULL, sizeof (overlay_targ_degrade_t)	},
	{ OVERLAY_TARG_RESTORE, B_TRUE, B_FALSE,
		NULL, overlay_target_restore,
		NULL, sizeof (overlay_targ_id_t)	},
	{ OVERLAY_TARG_LOOKUP, B_FALSE, B_TRUE,
		NULL, overlay_target_lookup_request,
		NULL, sizeof (overlay_targ_lookup_t)	},
	{ OVERLAY_TARG_RESPOND, B_TRUE, B_FALSE,
		NULL, overlay_target_lookup_respond,
		NULL, sizeof (overlay_targ_resp_t)	},
	{ OVERLAY_TARG_DROP, B_TRUE, B_FALSE,
		NULL, overlay_target_lookup_drop,
		NULL, sizeof (overlay_targ_resp_t)	},
	{ OVERLAY_TARG_PKT, B_TRUE, B_TRUE,
		overlay_target_pkt_copyin,
		overlay_target_packet,
		overlay_target_pkt_copyout,
		sizeof (overlay_targ_pkt_t)		},
	{ OVERLAY_TARG_INJECT, B_TRUE, B_FALSE,
		overlay_target_pkt_copyin,
		overlay_target_inject,
		NULL, sizeof (overlay_targ_pkt_t)	},
	{ OVERLAY_TARG_RESEND, B_TRUE, B_FALSE,
		overlay_target_pkt_copyin,
		overlay_target_resend,
		NULL, sizeof (overlay_targ_pkt_t)	},
	{ OVERLAY_TARG_LIST, B_FALSE, B_TRUE,
		overlay_target_list_copyin,
		overlay_target_ioctl_list,
		overlay_target_list_copyout,
		sizeof (overlay_targ_list_t)		},
	{ OVERLAY_TARG_CACHE_GET, B_FALSE, B_TRUE,
		NULL, overlay_target_cache_get,
		NULL, sizeof (overlay_targ_cache_t)	},
	{ OVERLAY_TARG_CACHE_SET, B_TRUE, B_TRUE,
		NULL, overlay_target_cache_set,
		NULL, sizeof (overlay_targ_cache_t)	},
	{ OVERLAY_TARG_CACHE_REMOVE, B_TRUE, B_TRUE,
		NULL, overlay_target_cache_remove,
		NULL, sizeof (overlay_targ_cache_t)	},
	{ OVERLAY_TARG_CACHE_FLUSH, B_TRUE, B_TRUE,
		NULL, overlay_target_cache_flush,
		NULL, sizeof (overlay_targ_cache_t)	},
	{ OVERLAY_TARG_CACHE_ITER, B_FALSE, B_TRUE,
		overlay_target_cache_iter_copyin,
		overlay_target_cache_iter,
		overlay_target_cache_iter_copyout,
		sizeof (overlay_targ_cache_iter_t)		},
	{ 0 }
};

int
overlay_target_open(dev_t *devp, int flags, int otype, cred_t *credp)
{
	minor_t mid;
	overlay_target_hdl_t *thdl;

	if (secpolicy_dl_config(credp) != 0)
		return (EPERM);

	if (getminor(*devp) != OVERLAY_TARGET_MINOR)
		return (ENXIO);

	if (otype & OTYP_BLK)
		return (EINVAL);

	if (flags & ~(FREAD | FWRITE | FEXCL))
		return (EINVAL);

	if ((flags & FWRITE) &&
	    !(flags & FEXCL))
		return (EINVAL);

	if (!(flags & FREAD) && !(flags & FWRITE))
		return (EINVAL);

	if (crgetzoneid(credp) != GLOBAL_ZONEID)
		return (EPERM);

	mid = id_alloc(overlay_thdl_idspace);
	if (ddi_soft_state_zalloc(overlay_thdl_state, mid) != 0) {
		id_free(overlay_thdl_idspace, mid);
		return (ENXIO);
	}

	thdl = ddi_get_soft_state(overlay_thdl_state, mid);
	VERIFY(thdl != NULL);
	thdl->oth_minor = mid;
	thdl->oth_zoneid = crgetzoneid(credp);
	thdl->oth_oflags = flags;
	mutex_init(&thdl->oth_lock, NULL, MUTEX_DRIVER, NULL);
	list_create(&thdl->oth_outstanding, sizeof (overlay_target_entry_t),
	    offsetof(overlay_target_entry_t, ote_qlink));
	*devp = makedevice(getmajor(*devp), mid);

	mutex_enter(&overlay_target_lock);
	if ((flags & FEXCL) && overlay_target_excl == B_TRUE) {
		mutex_exit(&overlay_target_lock);
		list_destroy(&thdl->oth_outstanding);
		mutex_destroy(&thdl->oth_lock);
		ddi_soft_state_free(overlay_thdl_state, mid);
		id_free(overlay_thdl_idspace, mid);
		return (EEXIST);
	} else if ((flags & FEXCL) != 0) {
		VERIFY(overlay_target_excl == B_FALSE);
		overlay_target_excl = B_TRUE;
	}
	list_insert_tail(&overlay_thdl_list, thdl);
	mutex_exit(&overlay_target_lock);

	return (0);
}

/* ARGSUSED */
int
overlay_target_ioctl(dev_t dev, int cmd, intptr_t arg, int mode, cred_t *credp,
    int *rvalp)
{
	overlay_target_ioctl_t *ioc = overlay_target_ioctab;
	overlay_target_hdl_t *thdl;

	if (secpolicy_dl_config(credp) != 0)
		return (EPERM);

	if ((thdl = ddi_get_soft_state(overlay_thdl_state,
	    getminor(dev))) == NULL)
		return (ENXIO);

	for (; ioc->oti_cmd != 0; ioc++) {
		int ret;
		caddr_t buf;
		size_t bufsize;

		if (ioc->oti_cmd != cmd)
			continue;

		if (ioc->oti_write == B_TRUE && !(mode & FWRITE))
			return (EBADF);

		if (ioc->oti_copyin == NULL) {
			bufsize = ioc->oti_size;
			buf = kmem_alloc(bufsize, KM_SLEEP);
			if (ddi_copyin((void *)(uintptr_t)arg, buf, bufsize,
			    mode & FKIOCTL) != 0) {
				kmem_free(buf, bufsize);
				return (EFAULT);
			}
		} else {
			if ((ret = ioc->oti_copyin((void *)(uintptr_t)arg,
			    (void **)&buf, &bufsize, mode)) != 0)
				return (ret);
		}

		ret = ioc->oti_func(thdl, buf);
		if (ret == 0 && ioc->oti_size != 0 &&
		    ioc->oti_ncopyout == B_TRUE) {
			if (ioc->oti_copyout == NULL) {
				if (ddi_copyout(buf, (void *)(uintptr_t)arg,
				    bufsize, mode & FKIOCTL) != 0)
					ret = EFAULT;
			} else {
				ret = ioc->oti_copyout((void *)(uintptr_t)arg,
				    buf, bufsize, mode);
			}
		}

		kmem_free(buf, bufsize);
		return (ret);
	}

	return (ENOTTY);
}

/* ARGSUSED */
int
overlay_target_close(dev_t dev, int flags, int otype, cred_t *credp)
{
	overlay_target_hdl_t *thdl;
	overlay_target_entry_t *entry;
	minor_t mid = getminor(dev);

	if ((thdl = ddi_get_soft_state(overlay_thdl_state, mid)) == NULL)
		return (ENXIO);

	mutex_enter(&overlay_target_lock);
	list_remove(&overlay_thdl_list, thdl);
	mutex_enter(&thdl->oth_lock);
	while ((entry = list_remove_head(&thdl->oth_outstanding)) != NULL)
		list_insert_tail(&overlay_target_list, entry);
	cv_signal(&overlay_target_condvar);
	mutex_exit(&thdl->oth_lock);
	if ((thdl->oth_oflags & FEXCL) != 0) {
		VERIFY(overlay_target_excl == B_TRUE);
		overlay_target_excl = B_FALSE;
	}
	mutex_exit(&overlay_target_lock);

	list_destroy(&thdl->oth_outstanding);
	mutex_destroy(&thdl->oth_lock);
	mid = thdl->oth_minor;
	ddi_soft_state_free(overlay_thdl_state, mid);
	id_free(overlay_thdl_idspace, mid);

	return (0);
}

/*
 * Return a pointer to the byte offset 'offset' within mp, traversing any
 * b_cont fragments as necessary. Sets remaining bytes in the mblk_t containing
 * 'offset' in *lenp. If offset is past the end of the packet, returns NULL.
 */
static void *
overlay_mblk_offset(mblk_t **restrict mpp, size_t offset, size_t *restrict lenp)
{
	mblk_t *mp = *mpp;
	size_t len = MBLKL(mp);

	while (offset >= len) {
		offset -= len;

		if ((mp = mp->b_cont) == NULL)
			return (NULL);
		len = MBLKL(mp);
	}

	*lenp = len - offset;
	*mpp = mp;
	return (mp->b_rptr + offset);
}

typedef enum overlay_ip6_res {
	OIP6_OK,
	OIP6_FRAGMENT,
	OIP6_TRUNCATED,
	OIP6_PULLUP
} overlay_ip6_res_t;

/*
 * Locate the L3 header in an IPv6 packet. mpp is the ptr to the address of
 * the mblk_t that contains the fixed portion of the IPv6 header, hdrp points
 * to the start of the IPv6 header (within *mpp).
 *
 * *lenp is set to the length of the remaining data in the mblk_t containing
 * the start of the L3 header. *protop is set to the L3 protocol (TCP, UDP,
 * etc.).
 */
static overlay_ip6_res_t
overlay_ip6_l3(mblk_t **mpp, unsigned char *hdrp, size_t *restrict lenp,
    uint8_t *restrict protop, unsigned char **l3hdrpp)
{
	ip6_t *ip6;
	struct ip6_opt *ip6_opt;
	size_t offset;
	uint8_t len;
	uint8_t opt_type;

	ip6 = (ip6_t *)hdrp;
	offset = hdrp - (*mpp)->b_rptr;
	opt_type = ip6->ip6_nxt;
	len = sizeof (ip6_t);

	/*
	 * IPv6 commingles IPv6 options and L3 protocols (they share the
	 * same ID space).
	 */
	while (opt_type != IPPROTO_NONE) {
		switch (opt_type) {
		case IPPROTO_TCP:
		case IPPROTO_UDP:
		case IPPROTO_ICMPV6:
			goto done;
		case IPPROTO_FRAGMENT:
			/* Punt on fragments */
			return (OIP6_FRAGMENT);
		case IP6OPT_PAD1:
			offset++;
			break;
		default:
			/*
			 * The length of an IPv6 option does not include
			 * the type byte header, so we must include that to
			 * skip past the option
			 */
			offset += 2 + len;
			break;
		}

		/*
		 * The IPv6 option header consists of two 8-bit values
		 * (type, length), so ip6_opt can be safely dereferenced
		 * without alignment concerns, however we do require the
		 * option header is not split across mblk_ts.
		 */
		ip6_opt = overlay_mblk_offset(mpp, offset, lenp);
		if (ip6_opt == NULL)
			return (OIP6_TRUNCATED);
		if (*lenp < 2)
			return (OIP6_PULLUP);

		opt_type = ip6_opt->ip6o_type;
		len = ip6_opt->ip6o_len;
	}

done:
	*protop = opt_type;
	if (opt_type == IPPROTO_NONE)
		return (OIP6_TRUNCATED);

	*l3hdrpp = overlay_mblk_offset(mpp, offset, lenp);
	if (*l3hdrpp == NULL)
		return (OIP6_TRUNCATED);

	return (OIP6_OK);
}

/*
 * This fills out 'op' with the data in 'orig_mp'.
 *
 * On success, 0 is returned and op->op_mblk will point to the corresponding
 * mblk_t. If msgpullup() had to be used on 'mp', 'orig_mp' will be freed and
 * op->op_mblk will point to the pulled-up copy of orig_mp.
 *
 * On failure, a non-zero value is returned and 'orig_mp' is not modified.
 * Additionally, *reasonp is set to a static string describing the reason
 * for the failure (primairly for use for dtrace probes).
 */
int
overlay_pkt_init(overlay_pkt_t *restrict op, mac_handle_t restrict mh,
    mblk_t *restrict orig_mp, const char **reasonp)
{
	mblk_t *first_mp = orig_mp;
	mblk_t *mp;
	size_t len, l2_len, l3_len;
	int ret = 0;
	boolean_t looped = B_FALSE;

	*reasonp = NULL;

again:
	mp = first_mp;

	/*
	 * If we ever msgpullup(), we should only need to attempt this
	 * a second time.
	 */
	VERIFY(!looped);

	bzero(op, sizeof (*op));

	if ((ret = mac_vlan_header_info(mh, mp, &op->op_mhi)) != 0)
		return (ret);

	/*
	 * Sanitize the mblk. If any of L1, L2, or L3 headers are split
	 * mid-header, we msgpullup() the whole thing and use that instead,
	 * otherwise cache the start of each header.
	 */
	len = MBLKL(mp);

	if (op->op_mhi.mhi_hdrsize > len) {
		OVERLAY_PULLUPMSG(orig_mp, "split ethernet header");
		if ((first_mp = msgpullup(orig_mp, -1)) == NULL) {
			*reasonp = "msgpullup failed";
			return (ENOMEM);
		}

		looped = B_TRUE;
		goto again;
	}

	/* Set the L2 header address */
	op->op2_u.op2_char = overlay_mblk_offset(&mp,
	    op->op_mhi.mhi_hdrsize, &len);

	/*
	 * Verify the remaining length in the mblk_t that contains the start
	 * of the L2 header doesn't split it across mblk_ts.
	 */
	switch (OPKT_ETYPE(op)) {
	case ETHERTYPE_IP:
		l2_len = sizeof (ipha_t);
		break;
	case ETHERTYPE_IPV6:
		l2_len = sizeof (ip6_t);
		break;
	case ETHERTYPE_ARP:
		l2_len = 28;
		break;
	default:
		/*
		 * For any other types, we don't bother checking for any
		 * further segmentation of the data.
		 */
		goto done;
	}

	if (len < l2_len) {
		OVERLAY_PULLUPMSG(orig_mp, "L2 header is split");
		if ((first_mp = msgpullup(orig_mp, -1)) == NULL) {
			*reasonp = "msgpullup failed";
			return (ENOMEM);
		}

		looped = B_TRUE;
		goto again;
	}

	/*
	 * We started with l2_len as the size of the fixed portion of the
	 * IPv4/IPv6 header. Now that we're sure the fixed portion isn't
	 * split across mblk_ts, we reset it to the total length of the
	 * IPv4/IPv6 header (i.e. including options) to determine the
	 * start of the L3 header.
	 *
	 * Also copy in the source and destination addresses.
	 */
	switch (op->op_mhi.mhi_bindsap) {
	case ETHERTYPE_IP:
		l2_len = IPH_HDR_LENGTH(op->op2_u.op2_ipv4);

		/*
		 * Sanity check the length. It must be within the min and max
		 * values, but also, it cannot be greater then the total
		 * size of the IPv4 header + IP payload. Upper layer will
		 * check this as well, be we need to do it just so we
		 * don't try to interpret the TCP/UDP header at the
		 * wrong location.
		 */
		if (l2_len < IP_SIMPLE_HDR_LENGTH ||
		    l2_len > IP_MAX_HDR_LENGTH ||
		    l2_len > ntohs(op->op2_u.op2_ipv4->ipha_length)) {
			*reasonp = "IP header length invalid";
			ret = EINVAL;
			goto done;
		}

		op->op_l3proto = op->op2_u.op2_ipv4->ipha_protocol;
		op->op3_u.op3_char = overlay_mblk_offset(&mp,
		    op->op_mhi.mhi_hdrsize + l2_len, &len);

		IN6_IPADDR_TO_V4MAPPED(op->op2_u.op2_ipv4->ipha_dst,
		    &op->op_dstaddr);
		IN6_IPADDR_TO_V4MAPPED(op->op2_u.op2_ipv4->ipha_src,
		    &op->op_srcaddr);
		break;
	case ETHERTYPE_IPV6:
		/*
		 * overlay_ip6_l3() also sets op->op3_u.op3_char for us
		 * after skipping over any options.
		 */
		switch (overlay_ip6_l3(&mp, op->op2_u.op2_char, &len,
		    &op->op_l3proto, &op->op3_u.op3_char)) {
		case OIP6_OK:
			break;
		case OIP6_FRAGMENT:
			*reasonp = "fragmented IPv6 packet";
			ret = EBADMSG;
			goto done;
		case OIP6_TRUNCATED:
			*reasonp = "truncated IPv6 packet";
			ret = EINVAL;
			goto done;
		case OIP6_PULLUP:
			if ((first_mp = msgpullup(orig_mp, -1)) == NULL) {
				*reasonp = "msgpullup failed";
				ret = ENOMEM;
				goto done;
			}

			looped = B_TRUE;
			goto again;
		}

		bcopy(&op->op2_u.op2_ipv6->ip6_dst, &op->op_dstaddr,
		    sizeof (struct in6_addr));
		bcopy(&op->op2_u.op2_ipv6->ip6_src, &op->op_srcaddr,
		    sizeof (struct in6_addr));
		break;
	case ETHERTYPE_ARP:
		/* There is no L3 with ARP packets */
		if (len < 28) {
			*reasonp = "truncated ARP packet";
			ret = EINVAL;
			goto done;
		}

		/* We don't need to do anything else for ARP packets */
		goto done;
	}

	switch (op->op_l3proto) {
	case IPPROTO_TCP:
		l3_len = sizeof (struct tcphdra_s);
		break;
	case IPPROTO_UDP:
		l3_len = sizeof (struct udphdr);
		break;
	case IPPROTO_ICMP:
		if (op->op_mhi.mhi_bindsap != ETHERTYPE_IP) {
			*reasonp = "ICMP in non-IP packet";
			ret = EINVAL;
			goto done;
		}
		l3_len = ICMPH_SIZE;
		break;
	case IPPROTO_ICMPV6:
		if (op->op_mhi.mhi_bindsap != ETHERTYPE_IPV6) {
			*reasonp = "ICMPv6 in non-IPv6 packet";
			ret = EINVAL;
			goto done;
		}
		l3_len = ICMP6_MINLEN;
		break;
	default:
		l3_len = 0;
	}

	if (len < l3_len) {
		OVERLAY_PULLUPMSG(orig_mp, "L3 header is split");
		if ((first_mp = msgpullup(orig_mp, -1)) == NULL) {
			*reasonp = "msgpullup failed";
			ret = ENOMEM;
			goto done;
		}

		looped = B_TRUE;
		goto again;
	}

	/* Now set the src/dst port */
	switch (op->op_l3proto) {
	case IPPROTO_TCP:
		op->op_srcport = ntohs(op->op3_u.op3_tcp->tha_lport);
		op->op_dstport = ntohs(op->op3_u.op3_tcp->tha_fport);

		/* Whatever is left in this mblk + any remaining fragments */
		op->op_l3len = len + msgdsize(mp->b_cont);
		break;
	case IPPROTO_UDP:
		op->op_srcport = ntohs(op->op3_u.op3_udp->uh_sport);
		op->op_dstport = ntohs(op->op3_u.op3_udp->uh_dport);

		/* Whatever is left in this mblk + any remaining fragments */
		op->op_l3len = len + msgdsize(mp->b_cont);
		break;
	case IPPROTO_ICMPV6:
		/*
		 * For ICMPv6 packets, we require the entire ICMPv6 payload to
		 * be contiguous. This is largely to simplify neighbor
		 * discovery handling for the router IP.
		 */
		if (mp->b_cont != NULL) {
			OVERLAY_PULLUPMSG(orig_mp, "ICMPv6 pkt is split");
			if ((first_mp = msgpullup(orig_mp, -1)) == NULL) {
				*reasonp = "msgpullup failed";
				ret = ENOMEM;
				goto done;
			}

			looped = B_TRUE;
			goto again;
		}

		/*
		 * From the above contiguous requirement, this means len
		 * must be the size of the ICMPv6 data (including header).
		 */
		op->op_l3len = len;
		break;
	}

done:
	if (first_mp != orig_mp) {
		if (ret == 0) {
			/*
			 * We had to do a msgpullup(), but were otherwise
			 * successful. We free the original mblk_t, and
			 * use our pulled-up mblk (first_mp).
			 */
			OVERLAY_FREEMSG(orig_mp,
			    "freeing original mblk after pullup");
			freemsg(orig_mp);
		} else if (first_mp != NULL) {
			/*
			 * Even after doing a msgpullup(), there was some
			 * other problem. We're returning an error so
			 * dispose of our pulled-up msg and leave the
			 * original untouched.
			 */
			freemsg(first_mp);
		}
	}

	if (ret == 0)
		op->op_mblk = first_mp;

	return (ret);
}
