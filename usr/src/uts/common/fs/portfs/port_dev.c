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
 * Copyright 2019 Joyent, Inc.
 */

/*
 * Device Events Notification
 * --------------------------
 *
 * This file implements the generic part of the PORT_SOURCE_DEVICE event source.
 * It provides an interface for drivers to register a device instance as an
 * event source, to unregister it, and to actually send events on behalf of a
 * device. The driver needs to provide entry points to handle device-specific
 * functions. Two structures describe the state of the event source and an
 * associated object:
 *
 * port_dev_ops_t contains the interface version as first member, followed by a
 * set of pointers to driver entry points implementing the device-specific parts
 * of this event source. The event source will keep a hash table of these
 * structures indexed by the devices dev_info_t. To register or unregister a
 * device a set of two functions is used:
 *
 * - portfs_register_dev(dev_info_t *, const port_dev_ops_t)
 *   This function will register a device with the event source and store a
 *   pointer to the port_dev_ops_t in pd_ops_hash for later use. Multiple
 *   devices managed by the same driver may share a single port_dev_ops_t, but
 *   each must be registered separately.
 *
 * - portfs_unregister_dev(dev_info_t *)
 *   This function removes a device's port_dev_ops_t from pd_ops_hash.
 *
 * A port_dev_t holds the state of an associated object. It caches the userspace
 * object pointer, the users pid, the requested events, the devices dip, the
 * minor vnode, the port it is associated to, and a pointer for device-specific
 * data. In addition it will also always have a port_kevent_t ready for sending.
 *
 * Another interface is provided to portfs, consisting of functions to
 * initialize this event source, to associate and dissociate the event source
 * to/from a port, and a callback for individual events.
 *
 *
 * Association:
 *
 * A device to be associated with a port is described by a dev_obj_t structure
 * in user memory. This structure is versioned, besides the version number it
 * contains a file descriptor of a device minor node. Device-specific additional
 * data can follow immediately after the dev_obj_t, this is usually achieved by
 * embedding the dev_obj_t as the first member of a device-specific structure.
 *
 * When port_associate_dev() is called, the dev_obj_t passed by the user is
 * read and checked for validity. The file descriptor is used to find the
 * dev_info_t of the device that is associated on.
 *
 * Only one association on an object is permitted per port. A port_dev_t
 * corresponding to the object will be created to hold the state of that
 * association. The drivers pd_port_dev_fill() entry point is called to fill in
 * any device-specific data. The association is completed by calling into the
 * drivers pd_port_associate() entry point. Calling port_associate() again on
 * the same object can be used to inform the driver about changes in the events
 * requested, the user cookie, and device-specific data held in the object.
 *
 *
 * Dissociation:
 *
 * When port_dissociate_dev() is called it will call into the drivers
 * pd_port_dissociate() entry point to clean up any device-specific state.
 * The port_dev_t will be destroyed and all resources will be freed.
 *
 *
 * Sending events:
 *
 * Devices can send events using the port_dev_send_event() function, which
 * takes a port_dev_t and the events to be sent. Before sending the event to
 * the port this function will create a new port_kevent_t and hook it up to the
 * port_dev_t, ensuring that there will always be a port_kevent_t available
 * for sending.
 *
 * When an event is about to be received through port_get(), portfs will call
 * the callback function set in the port_kevent_t, port_dev_callback(). This
 * will in turn call the device-specific callback pd_port_callback() which can
 * do further device-specific processing before the event is delivered.
 *
 *
 * Driver entry points:
 *
 * - pd_port_dev_fill(port_dev_t *pd)
 *   This entry point is called when a port_dev_t is created during association,
 *   and when re-associating on an already associated object. The driver can use
 *   it to set up the device-specific pd_data member of theport_dev_t.
 *
 * - pd_port_dev_free(port_dev_t *pd)
 *   This entry point is called to free any resources that a previous call to
 *   pd_port_dev_fill() allocated.
 *
 * - pd_port_associate(port_dev_t *pd, int events, void *user)
 *   This entry point is called to carry out the device-specific parts of the
 *   association.
 *
 * - pd_port_dissociate(port_dev_t *pd)
 *   This entry point is called to carry out the device-specific parts of
 *   dissociating from an object.
 *
 * - pd_port_callback(port_dev_t *pd, int *events, pid_t pid, int flag,
 *       port_kevent_t *pkevp)
 *   This entry point is called when an event is about to be sent to the user,
 *   the port is closed, or dissociated. This should be used to free any
 *   resources allocated for the event and/or updating the object in userspace
 *   with additional data.
 */

#include <sys/types.h>
#include <sys/systm.h>
#include <sys/stat.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/sysmacros.h>
#include <sys/debug.h>
#include <sys/fs/snode.h>
#include <sys/modhash.h>
#include <sys/port_impl.h>
#include <sys/port_kernel.h>

#define	PD_OPS_HASH_NCHAINS		97
static mod_hash_t *pd_ops_hash;

/*
 * port_dev_t hash table management
 *
 * Every port has its own hash table of port_dev_t entries, keyed by the user
 * object pointer and the user pid. The hash table is kept in the portsrc_data
 * member of the PORT_SOURCE_DEVICE event source in the port.
 *
 * For each user object pointer a list_t of matching port_dev_t elements is
 * stored in the hash table. Only one port_dev_t is allowed for any object/pid
 * tuple.
 *
 * Finding and removing a port_dev_t from the hash table returns the port_dev_t
 * in a locked state.
 */
#define	PD_PORT_HASH_NCHAINS		13

typedef struct port_dev_hash {
	mod_hash_t *ph_hash;
	kmutex_t ph_lock;
} port_dev_hash_t;

static port_dev_hash_t *
port_dev_hash_get(port_t *pp)
{
	port_source_t *pse;

	pse = port_getsrc(pp, PORT_SOURCE_DEVICE);
	VERIFY(pse != NULL);

	return ((port_dev_hash_t *)pse->portsrc_data);
}

static void
port_dev_hash_destroy(port_t *pp)
{
	port_source_t *pse;
	port_dev_hash_t *pdhash;

	pse = port_getsrc(pp, PORT_SOURCE_DEVICE);
	VERIFY(pse != NULL);

	VERIFY(MUTEX_HELD(&pp->port_queue.portq_source_mutex));
	pdhash = (port_dev_hash_t *)pse->portsrc_data;
	pse->portsrc_data = NULL;

	if (pdhash != NULL) {
		mod_hash_destroy_hash(pdhash->ph_hash);
		mutex_destroy(&pdhash->ph_lock);
		kmem_free(pdhash, sizeof (port_dev_hash_t));
	}
}

static void
port_dev_hash_delete(port_dev_hash_t *pdhash, uintptr_t object, list_t *pl)
{
	VERIFY(list_is_empty(pl) != 0);
	VERIFY0(mod_hash_remove(pdhash->ph_hash, (void *)object,
	    (mod_hash_val_t *)pl));
	kmem_free(pl, sizeof (list_t));
}

static port_dev_t *
port_dev_hash_find_helper(port_dev_hash_t *pdhash, uintptr_t object, pid_t pid,
    list_t **pl)
{
	port_dev_t *pd;

	VERIFY(MUTEX_HELD(&pdhash->ph_lock));

	if (mod_hash_find(pdhash->ph_hash, (void *)object,
	    (mod_hash_val_t *)pl) != 0) {
		return (NULL);
	}

	for (pd = list_head(*pl); pd != NULL; pd = list_next(*pl, pd)) {
		mutex_enter(&pd->pd_lock);
		if (pd->pd_object == object &&
		    pd->pd_pid == pid) {
			/* return pd locked */
			return (pd);
		}
		mutex_exit(&pd->pd_lock);
	}

	return (NULL);
}

/*
 * Find the port_dev_t for this object pointer and pid in the hash table. Return
 * with the port_dev_t locked.
 */
static port_dev_t *
port_dev_hash_find(port_dev_hash_t *pdhash, uintptr_t object, pid_t pid)
{
	port_dev_t *pd;
	list_t *pl;

	mutex_enter(&pdhash->ph_lock);
	pd = port_dev_hash_find_helper(pdhash, object, pid, &pl);
	mutex_exit(&pdhash->ph_lock);

	return (pd);
}

/*
 * Find the port_dev_t for this object pointer and pid in the hash table and
 * remove it. Return with the port_dev_t locked.
 */
static port_dev_t *
port_dev_hash_remove(port_dev_hash_t *pdhash, uintptr_t object, pid_t pid)
{
	port_dev_t *pd;
	list_t *pl;

	mutex_enter(&pdhash->ph_lock);
	pd = port_dev_hash_find_helper(pdhash, object, pid, &pl);
	if (pd != NULL) {
		list_remove(pl, pd);
		if (list_is_empty(pl)) {
			port_dev_hash_delete(pdhash, object, pl);
		}
	}
	mutex_exit(&pdhash->ph_lock);

	return (pd);
}

/*
 * Insert the port_dev_t into the hash table. Return -1 if a port_dev_t with the
 * same object/pid is already in the hash table, return 0 on success.
 */
static int
port_dev_hash_insert(port_dev_hash_t *pdhash, port_dev_t *p_dev)
{
	list_t *pl;
	port_dev_t *pd;

	mutex_enter(&pdhash->ph_lock);

	if (mod_hash_find(pdhash->ph_hash, (void *)p_dev->pd_object,
	    (mod_hash_val_t *)&pl) != 0) {
		pl = kmem_zalloc(sizeof (list_t), KM_SLEEP);
		list_create(pl, sizeof (port_dev_t),
		    offsetof(port_dev_t, pd_list));

		VERIFY0(mod_hash_insert(pdhash->ph_hash,
		    (void *)p_dev->pd_object, (mod_hash_val_t *)pl));
	} else {
		for (pd = list_head(pl); pd != NULL; pd = list_next(pl, pd)) {
			mutex_enter(&pd->pd_lock);
			if (pd->pd_object == p_dev->pd_object &&
			    pd->pd_pid == p_dev->pd_pid) {
				mutex_exit(&pd->pd_lock);
				mutex_exit(&pdhash->ph_lock);
				return (-1);
			}
			mutex_exit(&pd->pd_lock);
		}
	}

	list_insert_tail(pl, p_dev);
	mutex_exit(&pdhash->ph_lock);
	return (0);
}

/*
 * port_close() calls this function to request the PORT_SOURCE_DEVICE source
 * remove/free all resources allocated and associated with the port.
 */
static void
port_dev_close(void *arg, int port, pid_t pid, int lastclose)
{
	port_t *pp = arg;

	if (lastclose == 1)
		port_dev_hash_destroy(pp);
}

static void
port_dev_destroy(port_dev_t *p_dev)
{
	VERIFY0(list_link_active(&p_dev->pd_list));

	p_dev->pd_ops->pd_port_dissociate(p_dev);
	p_dev->pd_ops->pd_port_dev_free(p_dev);

	/*
	 * Don't free the event here if it currently is in the portq,
	 * port_close_events() will free it later.
	 */
	if ((p_dev->pd_kev->portkev_flags & PORT_KEV_DONEQ) == 0)
		port_free_event_local(p_dev->pd_kev, 0);

	mutex_destroy(&p_dev->pd_lock);
	kmem_free(p_dev, sizeof (port_dev_t));
}

static void
port_dev_hash_dtor(mod_hash_val_t val)
{
	list_t *pl = val;
	port_dev_t *pd;

	for (pd = list_remove_head(pl); pd != NULL; pd = list_remove_head(pl))
		port_dev_destroy(pd);

	kmem_free(pl, sizeof (list_t));
}

static int
port_dev_associate_source(port_dev_hash_t **pdhashp, port_t *pp)
{
	port_dev_hash_t *pdhash;
	port_source_t *pse;
	int ret;

	/*
	 * Associate PORT_SOURCE_DEVICE with the port if it is not associated
	 * yet. Note the PORT_SOURCE_DEVICE source is associated once and will
	 * not be dissociated.
	 */
	if ((pse = port_getsrc(pp, PORT_SOURCE_DEVICE)) == NULL) {
		ret = port_associate_ksource(pp->port_fd, PORT_SOURCE_DEVICE,
		    &pse, port_dev_close, pp, NULL);
		if (ret != 0) {
			*pdhashp = NULL;
			return (ret);
		}
	}

	/*
	 * Get the port_dev hash table. Create it if necessary.
	 */
	mutex_enter(&pp->port_queue.portq_source_mutex);

	if (pse->portsrc_data != NULL) {
		*pdhashp = pse->portsrc_data;
		mutex_exit(&pp->port_queue.portq_source_mutex);
		return (0);
	}

	pdhash = kmem_zalloc(sizeof (port_dev_hash_t), KM_SLEEP);
	mutex_init(&pdhash->ph_lock, NULL, MUTEX_DEFAULT, NULL);
	pdhash->ph_hash = mod_hash_create_ptrhash("portfs pd_port_hash",
	    PD_PORT_HASH_NCHAINS, port_dev_hash_dtor, sizeof (port_dev_t *));

	pse->portsrc_data = pdhash;
	mutex_exit(&pp->port_queue.portq_source_mutex);

	*pdhashp = pdhash;
	return (0);
}

static int
port_dev_callback(void *arg, int *events, pid_t pid, int flag, void *evp)
{
	port_kevent_t *pkevp = evp;
	port_dev_t *p_dev = arg;
	int error = 0;

	if (flag == PORT_CALLBACK_CLOSE) {
		/*
		 * The port is being closed. We must assume our port_dev_t
		 * has already been freed, so just return without calling
		 * the driver callback.
		 */
		return (0);
	}

	if (flag == PORT_CALLBACK_DEFAULT) {
		/*
		 * Event will be delivered to the application.
		 */
		if (curproc->p_pid != pid)
			return (EACCES);

		*events = pkevp->portkev_events;
		pkevp->portkev_events = 0;
	} else if (flag == PORT_CALLBACK_DISSOCIATE) {
		/*
		 * The object will be dissociated from the port.
		 */
		;
	} else {
		return (EINVAL);
	}

	mutex_enter(&p_dev->pd_lock);
	error = p_dev->pd_ops->pd_port_callback(p_dev, events, pid, flag,
	    pkevp);
	mutex_exit(&p_dev->pd_lock);

	return (error);
}

static int
port_dev_setup(port_t *pp, dev_info_t *dip, vnode_t *vp,
    port_dev_hash_t *pdhash, uintptr_t object, int events, void *user)
{
	port_dev_ops_t *p_ops;
	port_dev_t *p_dev;
	port_kevent_t *pkevp;
	int ret;

	/*
	 * If there is an existing association of this object with this port,
	 * update events and user and give the device a chance to update device-
	 * specific data.
	 */
	p_dev = port_dev_hash_find(pdhash, object, curproc->p_pid);

	if (p_dev != NULL) {
		p_dev->pd_events = events;
		p_dev->pd_kev->portkev_user = user;
		ret = p_dev->pd_ops->pd_port_dev_fill(p_dev);
		mutex_exit(&p_dev->pd_lock);
		return (ret);
	}

	/*
	 * Make sure we have a port_dev_ops for this device.
	 */
	if (mod_hash_find(pd_ops_hash, dip, (mod_hash_val_t *)&p_ops) != 0)
		return (ENODEV);

	/*
	 * Create a port_kevent_t first.
	 */
	if ((ret = port_alloc_event_local(pp, PORT_SOURCE_DEVICE,
	    PORT_ALLOC_DEFAULT, &pkevp)) != 0)
		return (ret);

	/*
	 * Allocate the port_dev_t for this object.
	 */
	p_dev = kmem_zalloc(sizeof (port_dev_t), KM_SLEEP);
	mutex_init(&p_dev->pd_lock, NULL, MUTEX_DEFAULT, NULL);
	p_dev->pd_object = object;
	p_dev->pd_dip = dip;
	p_dev->pd_vp = vp;
	p_dev->pd_port = pp;
	p_dev->pd_pid = curproc->p_pid;
	p_dev->pd_kev = pkevp;
	p_dev->pd_events = events;
	p_dev->pd_ops = p_ops;

	/*
	 * Initialize event. We use p_dev as argument to the callback.
	 */
	port_init_event(pkevp, object, user, port_dev_callback, p_dev);

	/*
	 * Let the device fill in pd_data.
	 */
	if ((ret = p_dev->pd_ops->pd_port_dev_fill(p_dev)) != 0) {
		mutex_destroy(&p_dev->pd_lock);
		port_free_event_local(pkevp, 0);
		kmem_free(p_dev, sizeof (port_dev_t));
		return (ret);
	}

	ret = port_dev_hash_insert(pdhash, p_dev);
	if (ret != 0) {
		p_dev->pd_ops->pd_port_dev_free(p_dev);
		mutex_destroy(&p_dev->pd_lock);
		port_free_event_local(pkevp, 0);
		kmem_free(p_dev, sizeof (port_dev_t));
		return (EEXIST);
	}

	ret = p_dev->pd_ops->pd_port_associate(p_dev, events, user);

	return (ret);
}

void
port_dev_send_event(port_dev_t *p_dev, int events)
{
	port_kevent_t *old, *new;

	VERIFY(MUTEX_HELD(&p_dev->pd_lock));

	old = p_dev->pd_kev;

	/*
	 * Allocate a new event to replace the one we send.
	 */
	if (port_dup_event(old, &new, PORT_ALLOC_DEFAULT) != 0)
		return;

	port_init_event(new, p_dev->pd_object, old->portkev_user,
	    port_dev_callback, p_dev);
	p_dev->pd_kev = new;

	old->portkev_events = events;
	port_send_event(old);
}

int
port_associate_dev(port_t *pp, int source, uintptr_t object, int events,
    void *user)
{
	port_dev_hash_t *pdhash;
	dev_info_t *dip;
	vnode_t *vp;
	file_t *fp;
	dev_obj_t dp;
	int ret;

	ASSERT(source == PORT_SOURCE_DEVICE);

	if (copyin((void *)object, &dp, sizeof (dev_obj_t)) != 0)
		return (EFAULT);

	if (dp.do_version != PORT_DEVICE_VERSION_1)
		return (EINVAL);

	if (dp.do_fd < 0)
		return (EBADFD);

	if ((fp = getf(dp.do_fd)) == NULL)
		return (EBADFD);

	vp = fp->f_vnode;

	if (vp->v_type != VBLK && vp->v_type != VCHR) {
		releasef(dp.do_fd);
		return (ENODEV);
	}

	if ((dip = spec_hold_devi_by_vp(vp)) == NULL) {
		releasef(dp.do_fd);
		return (ENODEV);
	}

	/* make sure the port is associated with PORT_SOURCE_DEVICE */
	if (port_dev_associate_source(&pdhash, pp) != 0) {
		ddi_release_devi(dip);
		releasef(dp.do_fd);
		return (ENODEV);
	}

	if ((ret = port_dev_setup(pp, dip, vp, pdhash, object, events,
	    user)) != 0) {
		ddi_release_devi(dip);
		releasef(dp.do_fd);
		return (ret);
	}

	ddi_release_devi(dip);
	releasef(dp.do_fd);

	return (ret);
}

int
port_dissociate_dev(port_t *pp, uintptr_t object)
{
	port_dev_t *p_dev;
	port_dev_hash_t *pdhash;
	int ret;

	pdhash = port_dev_hash_get(pp);
	p_dev = port_dev_hash_remove(pdhash, object, curproc->p_pid);
	if (p_dev == NULL)
		return (ENODEV);

	p_dev->pd_ops->pd_port_dissociate(p_dev);
	mutex_exit(&p_dev->pd_lock);

	port_dev_destroy(p_dev);

	return (0);
}

void
port_dev_init(void)
{
	pd_ops_hash = mod_hash_create_ptrhash("portfs pd_ops_hash",
	    PD_OPS_HASH_NCHAINS, mod_hash_null_valdtor,
	    sizeof (port_dev_ops_t *));

	/*
	 * mod_hash_create_ptrhash() is guaranteed not to fail due to use of
	 * sleeping allocations.
	 */
	ASSERT(pd_ops_hash != NULL);
}

int
portfs_register_dev(dev_info_t *dip, const port_dev_ops_t *p_ops)
{
	if (p_ops->pd_version != PORT_DEVICE_VERSION_1)
		return (EINVAL);

	if (p_ops->pd_port_dev_fill == NULL ||
	    p_ops->pd_port_dev_free == NULL ||
	    p_ops->pd_port_associate == NULL ||
	    p_ops->pd_port_dissociate == NULL ||
	    p_ops->pd_port_callback == NULL)
		return (EINVAL);

	if (mod_hash_insert(pd_ops_hash, dip, (mod_hash_val_t)p_ops) != 0)
		return (EINVAL);

	return (0);
}

void
portfs_unregister_dev(dev_info_t *dip)
{
	int ret;
	port_dev_ops_t *p_ops;

	ret = mod_hash_remove(pd_ops_hash, dip, (mod_hash_val_t *)&p_ops);

	ASSERT(ret == 0);
}
