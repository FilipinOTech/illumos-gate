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
 * Copyright (c) 2015 Joyent, Inc.  All rights reserved.
 */

/*
 * XXX
 */

#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/stat.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/modctl.h>
#include <sys/policy.h>
#include <sys/stream.h>
#include <sys/strsubr.h>
#include <sys/strsun.h>
#include <sys/types.h>
#include <sys/kmem.h>
#include <sys/param.h>
#include <sys/sysmacros.h>
#include <sys/ddifm.h>

#include <sys/dls.h>
#include <sys/dld_ioc.h>
#include <sys/mac_provider.h>
#include <sys/mac_client_priv.h>
#include <sys/mac_ether.h>
#include <sys/vlan.h>
/* XXX Should we really need this? */
#include <sys/socket.h>
#include <inet/ip.h>

#include <sys/overlay_impl.h>

dev_info_t *overlay_dip;
static kmutex_t overlay_dev_lock;
static list_t overlay_dev_list;
static uint8_t overlay_macaddr[ETHERADDRL] =
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

typedef enum overlay_dev_prop {
	OVERLAY_DEV_P_MTU = 0,
	OVERLAY_DEV_P_VNETID,
	OVERLAY_DEV_P_ENCAP,
	OVERLAY_DEV_P_VARPDID
} overlay_dev_prop_t;

#define	OVERLAY_DEV_NPROPS	4
static const char *overlay_dev_props[] = {
	"mtu",
	"vnetid",
	"encap",
	"varpd/id"
};

/*
 * XXX Dear Future rm, these are obviously kind of janky values, going from the
 * minimum size an IP implementation can handle to 100 less than MTU. While
 * having a default of 1400 is a bit unfortunate, it really does help things.
 * However, we should determine the largest MTU in a given netstack and use that
 * to bracket these. eg. min should be min(576, max(netstack mtus)-100) and max
 * should be max(netstack mtus) - 100).
 *
 * There are also some open questions on how we should handle interacting with
 * PMTU. I'd suggest that in general, we set the don't fragment bit on our
 * selves, or make that a property of a given protocol and then always ensure
 * that we listen for errors and emit them in a form of FMA ireport/erprort.
 */
#define	OVERLAY_MTU_MIN	576
#define	OVERLAY_MTU_DEF	1400
#define	OVERLAY_MTU_MAX	8900

overlay_dev_t *
overlay_hold_by_dlid(datalink_id_t id)
{
	overlay_dev_t *o;

	mutex_enter(&overlay_dev_lock);
	for (o = list_head(&overlay_dev_list); o != NULL;
	    o = list_next(&overlay_dev_list, o)) {
		if (id == o->odd_linkid) {
			mutex_enter(&o->odd_lock);
			o->odd_ref++;
			mutex_exit(&o->odd_lock);
			mutex_exit(&overlay_dev_lock);
			return (o);
		}
	}

	mutex_exit(&overlay_dev_lock);
	return (NULL);
}

void
overlay_hold_rele(overlay_dev_t *odd)
{
	mutex_enter(&odd->odd_lock);
	ASSERT(odd->odd_ref > 0);
	odd->odd_ref--;
	mutex_exit(&odd->odd_lock);
}

void
overlay_io_start(overlay_dev_t *odd, overlay_dev_flag_t flag)
{
	ASSERT(flag == OVERLAY_F_IN_RX || flag == OVERLAY_F_IN_TX);
	ASSERT(MUTEX_HELD(&odd->odd_lock));

	/* XXX Stat tracking */
	if (flag & OVERLAY_F_IN_RX)
		odd->odd_rxcount++;
	if (flag & OVERLAY_F_IN_TX)
		odd->odd_txcount++;
	odd->odd_flags |= flag;
}

void
overlay_io_done(overlay_dev_t *odd, overlay_dev_flag_t flag)
{
	boolean_t signal = B_FALSE;

	ASSERT(flag == OVERLAY_F_IN_RX || flag == OVERLAY_F_IN_TX);
	ASSERT(MUTEX_HELD(&odd->odd_lock));

	/* XXX Stat tracking */
	if (flag & OVERLAY_F_IN_RX) {
		ASSERT(odd->odd_rxcount > 0);
		odd->odd_rxcount--;
		if (odd->odd_rxcount == 0) {
			signal = B_TRUE;
			odd->odd_flags &= ~OVERLAY_F_IN_RX;
		}
	}
	if (flag & OVERLAY_F_IN_TX) {
		ASSERT(odd->odd_txcount > 0);
		odd->odd_txcount--;
		if (odd->odd_txcount == 0) {
			signal = B_TRUE;
			odd->odd_flags &= ~OVERLAY_F_IN_TX;
		}
	}

	/*
	 * XXX Work out semantics here so we don't have to broadcast.
	 */
	if (signal == B_TRUE)
		cv_broadcast(&odd->odd_iowait);
}

static void
overlay_io_wait(overlay_dev_t *odd, overlay_dev_flag_t flag)
{
	ASSERT((flag & ~OVERLAY_F_IOMASK) == 0);
	ASSERT(MUTEX_HELD(&odd->odd_lock));

	while (odd->odd_flags & flag) {
		cv_wait(&odd->odd_iowait, &odd->odd_lock);
	}
}

void
overlay_dev_iter(overlay_dev_iter_f func, void *arg)
{
	overlay_dev_t *odd;

	mutex_enter(&overlay_dev_lock);
	for (odd = list_head(&overlay_dev_list); odd != NULL;
	    odd = list_next(&overlay_dev_list, odd)) {
		if (func(odd, arg) != 0) {
			mutex_exit(&overlay_dev_lock);
			return;
		}
	}
	mutex_exit(&overlay_dev_lock);
}

static int
overlay_m_stat(void *arg, uint_t stat, uint64_t *val)
{
	return (ENOTSUP);
}

/*
 * XXX We should use this as a means of lazily opening the lower level
 * port that we end up using.
 */
static int
overlay_m_start(void *arg)
{
	overlay_dev_t *odd = arg;
	overlay_mux_t *mux;
	int ret, domain, family, prot;
	struct sockaddr_storage storage;
	socklen_t slen;

	mutex_enter(&odd->odd_lock);
	if ((odd->odd_flags & OVERLAY_F_ACTIVATED) == 0) {
		mutex_exit(&odd->odd_lock);
		return (EAGAIN);
	}
	mutex_exit(&odd->odd_lock);

	ret = odd->odd_plugin->ovp_ops->ovpo_socket(odd->odd_pvoid, &domain,
	    &family, &prot, (struct sockaddr *)&storage, &slen);
	if (ret != 0)
		return (ret);

	mux = overlay_mux_open(odd->odd_plugin, domain, family, prot,
	    (struct sockaddr *)&storage, slen, &ret);
	if (mux == NULL)
		return (ret);

	overlay_mux_add_dev(mux, odd);
	odd->odd_mux = mux;
	mutex_enter(&odd->odd_lock);
	ASSERT(!(odd->odd_flags & OVERLAY_F_IN_MUX));
	odd->odd_flags |= OVERLAY_F_IN_MUX;
	mutex_exit(&odd->odd_lock);

	return (0);
}

static void
overlay_m_stop(void *arg)
{
	overlay_dev_t *odd = arg;

	/*
	 * The MAC Perimeter is held here, so we don't have to worry about
	 * synchornizing this with respect to metadata operations.
	 */
	mutex_enter(&odd->odd_lock);
	VERIFY(odd->odd_flags & OVERLAY_F_IN_MUX);
	VERIFY(!(odd->odd_flags & OVERLAY_F_MDDROP));
	odd->odd_flags |= OVERLAY_F_MDDROP;
	overlay_io_wait(odd, OVERLAY_F_IOMASK);
	mutex_exit(&odd->odd_lock);

	overlay_mux_remove_dev(odd->odd_mux, odd);
	overlay_mux_close(odd->odd_mux);
	odd->odd_mux = NULL;

	mutex_enter(&odd->odd_lock);
	odd->odd_flags &= ~OVERLAY_F_IN_MUX;
	odd->odd_flags &= ~OVERLAY_F_MDDROP;
	VERIFY((odd->odd_flags & OVERLAY_F_STOPMASK) == 0);
	mutex_exit(&odd->odd_lock);
}

static int
overlay_m_promisc(void *arg, boolean_t on)
{
	/* XXX Is there anything for us to do for promisc, I don't think so */
	return (0);
}

static int
overlay_m_multicast(void *arg, boolean_t add, const uint8_t *addrp)
{
	/*
	 * XXX Semantically we support an unlimited number of multicast mac
	 * addresses. I think the real long term question is should this hit
	 * varpd for notification and/or approval? Likely only the former.
	 */
	return (0);
}

static int
overlay_m_unicast(void *arg, const uint8_t *macaddr)
{
	/*
	 * XXX Semantically we support an unlimited number of multicast mac
	 * addresses. I think the real long term question is should this hit
	 * varpd for notification and/or approval? Likely only the former.
	 */
	return (0);
}

mblk_t *
overlay_m_tx(void *arg, mblk_t *mp_chain)
{
	overlay_dev_t *odd = arg;
	mblk_t *mp, *ep;
	int ret;
	ovep_encap_info_t einfo;
	struct msghdr hdr;

	mutex_enter(&odd->odd_lock);
	if ((odd->odd_flags & OVERLAY_F_MDDROP) ||
	    !(odd->odd_flags & OVERLAY_F_IN_MUX)) {
		/* XXX Stats, etc. */
		mutex_exit(&odd->odd_lock);
		freemsgchain(mp_chain);
		return (NULL);
	}
	overlay_io_start(odd, OVERLAY_F_IN_TX);
	mutex_exit(&odd->odd_lock);

	bzero(&hdr, sizeof (struct msghdr));

	/* XXX Zero this out */
	einfo.ovdi_id = odd->odd_vid;
	mp = mp_chain;
	while (mp != NULL) {
		socklen_t slen;
		struct sockaddr_storage storage;

		mp_chain = mp->b_next;
		mp->b_next = NULL;
		ep = NULL;

		ret = overlay_target_lookup(odd, mp,
		    (struct sockaddr *)&storage, &slen);
		if (ret != OVERLAY_TARGET_OK) {
			if (ret == OVERLAY_TARGET_DROP)
				freemsg(mp);
			mp = mp_chain;
			continue;
		}

		hdr.msg_name = &storage;
		hdr.msg_namelen = slen;

		ret = odd->odd_plugin->ovp_ops->ovpo_encap(odd->odd_mh, mp,
		    &einfo, &ep);
		if (ret != 0 || ep == NULL) {
			freemsg(mp);
			goto out;
		}

		ep->b_cont = mp;
		ret = overlay_mux_tx(odd->odd_mux, &hdr, ep);
		if (ret != 0)
			goto out;

		mp = mp_chain;
	}

out:
	mutex_enter(&odd->odd_lock);
	overlay_io_done(odd, OVERLAY_F_IN_TX);
	mutex_exit(&odd->odd_lock);
	return (mp_chain);
}

static void
overlay_m_ioctl(void *arg, queue_t *q, mblk_t *mp)
{
	miocnak(q, mp, 0, ENOTSUP);
}

static boolean_t
overlay_m_getcapab(void *arg, mac_capab_t cap, void *cap_data)
{
	return (B_FALSE);
}

static int
overlay_m_setprop(void *arg, const char *pr_name, mac_prop_id_t pr_num,
    uint_t pr_valsize, const void *pr_val)
{
	uint32_t mtu, old;
	int err;
	overlay_dev_t *odd = arg;

	if (pr_num != MAC_PROP_MTU)
		return (ENOTSUP);

	bcopy(pr_val, &mtu, sizeof (mtu));
	if (mtu < OVERLAY_MTU_MIN || mtu > OVERLAY_MTU_MAX)
		return (EINVAL);

	mutex_enter(&odd->odd_lock);
	old = odd->odd_mtu;
	odd->odd_mtu = mtu;
	err = mac_maxsdu_update(odd->odd_mh, mtu);
	if (err != 0)
		odd->odd_mtu = old;
	mutex_exit(&odd->odd_lock);

	return (err);
}

/*
 * XXX Come back and figure out what other properties to emulate. Don't forget,
 * MAC handles a bunch of these for us.
 */
static int
overlay_m_getprop(void *arg, const char *pr_name, mac_prop_id_t pr_num,
    uint_t pr_valsize, void *pr_val)
{
	return (ENOTSUP);
}

static void
overlay_m_propinfo(void *arg, const char *pr_name, mac_prop_id_t pr_num,
    mac_prop_info_handle_t prh)
{
	if (pr_num != MAC_PROP_MTU)
		return;

	mac_prop_info_set_default_uint32(prh, OVERLAY_MTU_DEF);
	mac_prop_info_set_range_uint32(prh, OVERLAY_MTU_MIN, OVERLAY_MTU_MAX);
}

static mac_callbacks_t overlay_m_callbacks = {
	.mc_callbacks = (MC_IOCTL | MC_GETCAPAB | MC_SETPROP | MC_GETPROP |
	    MC_PROPINFO),
	.mc_getstat = overlay_m_stat,
	.mc_start = overlay_m_start,
	.mc_stop = overlay_m_stop,
	.mc_setpromisc = overlay_m_promisc,
	.mc_multicst = overlay_m_multicast,
	.mc_unicst = overlay_m_unicast,
	.mc_tx = overlay_m_tx,
	.mc_ioctl = overlay_m_ioctl,
	.mc_getcapab = overlay_m_getcapab,
	.mc_getprop = overlay_m_getprop,
	.mc_setprop = overlay_m_setprop,
	.mc_propinfo = overlay_m_propinfo
};

static boolean_t
overlay_valid_name(const char *name, size_t buflen)
{
	size_t actlen;
	int err, i;

	for (i = 0; i < buflen; i++) {
		if (name[i] == '\0')
			break;
	}

	if (i == 0 || i == buflen)
		return (B_FALSE);
	actlen = i;
	if (strchr(name, '/') != NULL)
		return (B_FALSE);
	if (u8_validate((char *)name, actlen, NULL,
	    U8_VALIDATE_ENTIRE, &err) < 0)
		return (B_FALSE);
	return (B_TRUE);
}

static int
overlay_i_create(void *karg, intptr_t arg, int mode, cred_t *cred, int *rvalp)
{
	int err;
	uint64_t maxid;
	overlay_dev_t *odd, *o;
	mac_register_t *mac;
	char name[MAXLINKNAMELEN];
	overlay_ioc_create_t *oicp = karg;

	if (overlay_valid_name(oicp->oic_encap, MAXLINKNAMELEN) == B_FALSE)
		return (EINVAL);

	odd = kmem_zalloc(sizeof (overlay_dev_t), KM_SLEEP);
	odd->odd_linkid = oicp->oic_linkid;
	odd->odd_plugin = overlay_plugin_lookup(oicp->oic_encap);
	if (odd->odd_plugin == NULL) {
		kmem_free(odd, sizeof (overlay_dev_t));
		/* XXX Better errno */
		return (ENOENT);
	}
	err = odd->odd_plugin->ovp_ops->ovpo_init((overlay_handle_t)odd,
	    &odd->odd_pvoid);
	if (err != 0) {
		odd->odd_plugin->ovp_ops->ovpo_fini(odd->odd_pvoid);
		overlay_plugin_rele(odd->odd_plugin);
		kmem_free(odd, sizeof (overlay_dev_t));
		/* XXX Better errno */
		return (EINVAL);
	}

	/*
	 * Make sure that our virtual network id is valid for the given plugin
	 * that we're working with.
	 */
	ASSERT(odd->odd_plugin->ovp_id_size <= 8);
	maxid = UINT64_MAX;
	if (odd->odd_plugin->ovp_id_size != 8)
		maxid = (1ULL << (odd->odd_plugin->ovp_id_size * 8)) - 1ULL;
	if (oicp->oic_vnetid > maxid) {
		odd->odd_plugin->ovp_ops->ovpo_fini(odd->odd_pvoid);
		overlay_plugin_rele(odd->odd_plugin);
		kmem_free(odd, sizeof (overlay_dev_t));
		/* XXX Better errno */
		return (EINVAL);
	}
	odd->odd_vid = oicp->oic_vnetid;

	mac = mac_alloc(MAC_VERSION);
	if (mac == NULL) {
		mutex_exit(&overlay_dev_lock);
		odd->odd_plugin->ovp_ops->ovpo_fini(odd->odd_pvoid);
		overlay_plugin_rele(odd->odd_plugin);
		kmem_free(odd, sizeof (overlay_dev_t));
		return (EINVAL);
	}

	/* TODO These are always good props */
	mac->m_type_ident = MAC_PLUGIN_IDENT_ETHER;
	mac->m_driver = odd;
	mac->m_dip = overlay_dip;
	mac->m_dst_addr = NULL;
	mac->m_callbacks = &overlay_m_callbacks;
	mac->m_pdata = NULL;
	mac->m_pdata_size = 0;

	/* XXX This will almost certainly change */
	mac->m_priv_props = NULL;

	/* XXX I think we should let mac handle this itself */
	mac->m_instance = (uint_t)-1;

	/*
	 * XXX This is definitely wrong. There is no real source address that
	 * should be used here, but saying that we're not ethernet is going to
	 * cause its own problems.
	 */
	mac->m_src_addr = overlay_macaddr;

	/*
	 * XXX These should come from the underlying device that we've been
	 * created over and be influenced based on the encap method.
	 */
	mac->m_min_sdu = 1;
	mac->m_max_sdu = 1400;

	/*
	 * XXX This needs to come from the encapsulation protocol, well, really,
	 * we'll ask if it it supports vlans then go from there. In this case
	 * the underlying device doesn't matter, since it will be encapsulated.
	 */
	mac->m_margin = VLAN_TAGSZ;

	/*
	 * XXX While it seems like we should say that we have no virt
	 * assistence, we should figure out what that implies elsewhere and
	 * whether we can leverage existing software.
	 */
	mac->m_v12n = MAC_VIRT_NONE;

	/*
	 * XXX I'm not sure that we should bother emulating a separate multicast
	 * sdu. While we will have to take it into account in our min and max
	 * sdu that we give because some of our traffic may be arbitrarily sent
	 * over multicast, I'm pretty sure vnics above us don't need it.
	 */
	mac->m_multicast_sdu = 0;

	mutex_enter(&overlay_dev_lock);
	for (o = list_head(&overlay_dev_list); o != NULL;
	    o = list_next(&overlay_dev_list, o)) {
		if (o->odd_linkid == oicp->oic_linkid) {
			mutex_exit(&overlay_dev_lock);
			odd->odd_plugin->ovp_ops->ovpo_fini(odd->odd_pvoid);
			overlay_plugin_rele(odd->odd_plugin);
			kmem_free(odd, sizeof (overlay_dev_t));
			return (EEXIST);
		}

		if (o->odd_vid == oicp->oic_vnetid &&
		    o->odd_plugin == odd->odd_plugin) {
			mutex_exit(&overlay_dev_lock);
			odd->odd_plugin->ovp_ops->ovpo_fini(odd->odd_pvoid);
			overlay_plugin_rele(odd->odd_plugin);
			kmem_free(odd, sizeof (overlay_dev_t));
			return (EEXIST);
		}
	}

	err = mac_register(mac, &odd->odd_mh);
	mac_free(mac);
	if (err != 0) {
		mutex_exit(&overlay_dev_lock);
		odd->odd_plugin->ovp_ops->ovpo_fini(odd->odd_pvoid);
		overlay_plugin_rele(odd->odd_plugin);
		kmem_free(odd, sizeof (overlay_dev_t));
		return (err);
	}

	err = dls_devnet_create(odd->odd_mh, odd->odd_linkid,
	    crgetzoneid(cred));
	if (err != 0) {
		mutex_exit(&overlay_dev_lock);
		(void) mac_unregister(odd->odd_mh);
		odd->odd_plugin->ovp_ops->ovpo_fini(odd->odd_pvoid);
		overlay_plugin_rele(odd->odd_plugin);
		kmem_free(odd, sizeof (overlay_dev_t));
		return (err);
	}

	mutex_init(&odd->odd_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&odd->odd_iowait, NULL, CV_DRIVER, NULL);
	odd->odd_ref = 0;
	odd->odd_flags = 0;
	list_insert_tail(&overlay_dev_list, odd);
	mutex_exit(&overlay_dev_lock);

	return (0);
}

static int
overlay_i_activate(void *karg, intptr_t arg, int mode, cred_t *cred, int *rvalp)
{
	int i, ret;
	overlay_dev_t *odd;
	mac_perim_handle_t mph;
	overlay_ioc_activate_t *oiap = karg;
	overlay_ioc_propinfo_t *infop;
	overlay_ioc_prop_t *oip;
	overlay_prop_handle_t phdl;

	odd = overlay_hold_by_dlid(oiap->oia_linkid);
	if (odd == NULL)
		return (ENOENT);

	infop = kmem_alloc(sizeof (overlay_ioc_propinfo_t), KM_SLEEP);
	oip = kmem_alloc(sizeof (overlay_ioc_prop_t), KM_SLEEP);
	phdl = (overlay_prop_handle_t)infop;

	mac_perim_enter_by_mh(odd->odd_mh, &mph);
	mutex_enter(&odd->odd_lock);
	if (odd->odd_flags & OVERLAY_F_ACTIVATED) {
		mutex_exit(&odd->odd_lock);
		mac_perim_exit(mph);
		overlay_hold_rele(odd);
		kmem_free(infop, sizeof (overlay_ioc_propinfo_t));
		kmem_free(oip, sizeof (overlay_ioc_prop_t));
		return (EEXIST);
	}
	mutex_exit(&odd->odd_lock);

	for (i = 0; i < odd->odd_plugin->ovp_nprops; i++) {
		const char *pname = odd->odd_plugin->ovp_props[i];
		bzero(infop, sizeof (overlay_ioc_propinfo_t));
		overlay_prop_init(phdl);
		ret = odd->odd_plugin->ovp_ops->ovpo_propinfo(pname, phdl);
		if (ret != 0) {
			mac_perim_exit(mph);
			overlay_hold_rele(odd);
			kmem_free(infop, sizeof (overlay_ioc_propinfo_t));
			kmem_free(oip, sizeof (overlay_ioc_prop_t));
			return (ret);
		}

		if ((infop->oipi_prot & OVERLAY_PROP_PERM_REQ) == 0)
			continue;
		bzero(oip, sizeof (overlay_ioc_prop_t));
		oip->oip_size = sizeof (oip->oip_value);
		ret = odd->odd_plugin->ovp_ops->ovpo_getprop(odd->odd_pvoid,
		    pname, oip->oip_value, &oip->oip_size);
		if (ret != 0) {
			mac_perim_exit(mph);
			overlay_hold_rele(odd);
			kmem_free(infop, sizeof (overlay_ioc_propinfo_t));
			kmem_free(oip, sizeof (overlay_ioc_prop_t));
			return (ret);
		}
		if (oip->oip_size == 0) {
			mac_perim_exit(mph);
			overlay_hold_rele(odd);
			kmem_free(infop, sizeof (overlay_ioc_propinfo_t));
			kmem_free(oip, sizeof (overlay_ioc_prop_t));
			return (EINVAL);
		}
	}

	mutex_enter(&odd->odd_lock);
	if ((odd->odd_flags & OVERLAY_F_VARPD) == 0) {
		mutex_exit(&odd->odd_lock);
		mac_perim_exit(mph);
		overlay_hold_rele(odd);
		kmem_free(infop, sizeof (overlay_ioc_propinfo_t));
		kmem_free(oip, sizeof (overlay_ioc_prop_t));
		return (ENXIO);
	}

	ASSERT((odd->odd_flags & OVERLAY_F_ACTIVATED) == 0);
	odd->odd_flags |= OVERLAY_F_ACTIVATED;
	mutex_exit(&odd->odd_lock);

	mac_perim_exit(mph);
	overlay_hold_rele(odd);
	kmem_free(infop, sizeof (overlay_ioc_propinfo_t));
	kmem_free(oip, sizeof (overlay_ioc_prop_t));

	return (0);
}

static int
overlay_i_delete(void *karg, intptr_t arg, int mode, cred_t *cred, int *rvalp)
{
	overlay_ioc_delete_t *oidp = karg;
	overlay_dev_t *odd;
	datalink_id_t tid;
	int ret;

	/*
	 * XXX Need to better understand the semantics of what gld is doing with
	 * respect to our data structure... This probably needs to be entirely
	 * reworked.
	 */
	odd = overlay_hold_by_dlid(oidp->oid_linkid);
	if (odd == NULL) {
		return (ENOENT);
	}

	mutex_enter(&odd->odd_lock);
	/* If we're not the only hold, we're busy */
	if (odd->odd_ref != 1) {
		mutex_exit(&odd->odd_lock);
		overlay_hold_rele(odd);
		return (EBUSY);
	}

	if (odd->odd_flags & OVERLAY_F_IN_MUX) {
		mutex_exit(&odd->odd_lock);
		overlay_hold_rele(odd);
		return (EBUSY);
	}

	/*
	 * To remove this, we need to first remove it from dls and then remove
	 * it from mac. The act of removing it from mac will check if there are
	 * devices on top of this, eg. vnics. If there are, then that will fail
	 * and we'll have to go through and recreate the dls entry. Only after
	 * mac_unregister has succeeded, then we'll go through and actually free
	 * everything and drop the dev lock.
	 */
	ret = dls_devnet_destroy(odd->odd_mh, &tid, B_TRUE);
	if (ret != 0) {
		overlay_hold_rele(odd);
		return (ret);
	}

	ASSERT(oidp->oid_linkid == tid);
	ret = mac_disable(odd->odd_mh);
	if (ret != 0) {
		(void) dls_devnet_create(odd->odd_mh, odd->odd_linkid,
		    crgetzoneid(cred));
		overlay_hold_rele(odd);
		return (ret);
	}

	overlay_target_quiesce(odd->odd_target);

	mutex_enter(&overlay_dev_lock);
	list_remove(&overlay_dev_list, odd);
	mutex_exit(&overlay_dev_lock);

	cv_destroy(&odd->odd_iowait);
	mutex_destroy(&odd->odd_lock);
	overlay_target_free(odd);
	odd->odd_plugin->ovp_ops->ovpo_fini(odd->odd_pvoid);
	overlay_plugin_rele(odd->odd_plugin);
	kmem_free(odd, sizeof (overlay_dev_t));

	return (0);
}

static int
overlay_i_nprops(void *karg, intptr_t arg, int mode, cred_t *cred,
    int *rvalp)
{
	overlay_dev_t *odd;
	overlay_ioc_nprops_t *on = karg;

	odd = overlay_hold_by_dlid(on->oipn_linkid);
	if (odd == NULL)
		return (ENOENT);
	on->oipn_nprops = odd->odd_plugin->ovp_nprops + OVERLAY_DEV_NPROPS;
	overlay_hold_rele(odd);

	return (0);
}

static int
overlay_propinfo_plugin_cb(overlay_plugin_t *opp, void *arg)
{
	overlay_prop_handle_t phdl = arg;
	overlay_prop_set_range_str(phdl, opp->ovp_name);
	return (0);
}

static int
overlay_i_name_to_propid(overlay_dev_t *odd, const char *name, uint_t *id)
{
	int i;

	for (i = 0; i < OVERLAY_DEV_NPROPS; i++) {
		if (strcmp(overlay_dev_props[i], name) == 0) {
			*id = i;
			return (0);
		}
	}

	for (i = 0; i < odd->odd_plugin->ovp_nprops; i++) {
		if (strcmp(odd->odd_plugin->ovp_props[i], name) == 0) {
			*id = i + OVERLAY_DEV_NPROPS;
			return (0);
		}
	}

	return (ENOENT);
}

static void
overlay_i_propinfo_mtu(overlay_dev_t *odd, overlay_prop_handle_t phdl)
{
	uint32_t def;
	mac_propval_range_t range;
	uint_t perm;

	ASSERT(MAC_PERIM_HELD(odd->odd_mh));

	bzero(&range, sizeof (mac_propval_range_t));
	range.mpr_count = 1;
	if (mac_prop_info(odd->odd_mh, MAC_PROP_MTU, "mtu", &def,
	    sizeof (def), &range, &perm) != 0)
		return;

	if (perm == MAC_PROP_PERM_READ)
		overlay_prop_set_prot(phdl, OVERLAY_PROP_PERM_READ);
	else if (perm == MAC_PROP_PERM_WRITE)
		overlay_prop_set_prot(phdl, OVERLAY_PROP_PERM_WRITE);
	else if (perm == MAC_PROP_PERM_RW)
		overlay_prop_set_prot(phdl, OVERLAY_PROP_PERM_RW);

	overlay_prop_set_type(phdl, OVERLAY_PROP_T_UINT);
	overlay_prop_set_default(phdl, &def, sizeof (def));
	overlay_prop_set_range_uint32(phdl, range.mpr_range_uint32[0].mpur_min,
	    range.mpr_range_uint32[0].mpur_max);
}

static int
overlay_i_propinfo(void *karg, intptr_t arg, int mode, cred_t *cred,
    int *rvalp)
{
	overlay_dev_t *odd;
	const char *pname;
	int ret;
	mac_perim_handle_t mph;
	uint_t propid = UINT_MAX;
	overlay_ioc_propinfo_t *oip = karg;
	overlay_prop_handle_t phdl = (overlay_prop_handle_t)oip;

	odd = overlay_hold_by_dlid(oip->oipi_linkid);
	if (odd == NULL)
		return (ENOENT);

	overlay_prop_init(phdl);
	mac_perim_enter_by_mh(odd->odd_mh, &mph);

	/*
	 * If the id is -1, then the property that we're looking for is named in
	 * oipi_name and we should fill in its id. Otherwise, we've been given
	 * an id and we need to turn that into a name for our plugin's sake. The
	 * id is our own fabrication for property discovery.
	 */
	if (oip->oipi_id == -1) {
		int i;

		/*
		 * Determine if it's a known generic property or it belongs to a
		 * module by checking against the list of known names.
		 */
		oip->oipi_name[OVERLAY_PROP_NAMELEN-1] = '\0';
		if ((ret = overlay_i_name_to_propid(odd, oip->oipi_name,
		    &propid)) != 0) {
			overlay_hold_rele(odd);
			mac_perim_exit(mph);
			return (ret);
		}
		oip->oipi_id = propid;
		if (propid >= OVERLAY_DEV_NPROPS) {
			ret = odd->odd_plugin->ovp_ops->ovpo_propinfo(
			    oip->oipi_name, phdl);
			overlay_hold_rele(odd);
			mac_perim_exit(mph);
			return (ret);

		}
	} else if (oip->oipi_id >= OVERLAY_DEV_NPROPS) {
		uint_t id = oip->oipi_id - OVERLAY_DEV_NPROPS;

		if (id >= odd->odd_plugin->ovp_nprops) {
			overlay_hold_rele(odd);
			mac_perim_exit(mph);
			return (EINVAL);
		}
		ret = odd->odd_plugin->ovp_ops->ovpo_propinfo(
		    odd->odd_plugin->ovp_props[id], phdl);
		overlay_hold_rele(odd);
		mac_perim_exit(mph);
		return (ret);
	} else if (oip->oipi_id < -1) {
		overlay_hold_rele(odd);
		mac_perim_exit(mph);
		return (EINVAL);
	} else {
		ASSERT(oip->oipi_id < OVERLAY_DEV_NPROPS);
		ASSERT(oip->oipi_id >= 0);
		propid = oip->oipi_id;
		(void) strlcpy(oip->oipi_name, overlay_dev_props[propid],
		    sizeof (oip->oipi_name));
	}

	switch (propid) {
	case OVERLAY_DEV_P_MTU:
		overlay_i_propinfo_mtu(odd, phdl);
		break;
	case OVERLAY_DEV_P_VNETID:
		overlay_prop_set_prot(phdl, OVERLAY_PROP_PERM_RW);
		overlay_prop_set_type(phdl, OVERLAY_PROP_T_UINT);
		overlay_prop_set_nodefault(phdl);
		break;
	case OVERLAY_DEV_P_ENCAP:
		overlay_prop_set_prot(phdl, OVERLAY_PROP_PERM_READ);
		overlay_prop_set_type(phdl, OVERLAY_PROP_T_STRING);
		overlay_prop_set_nodefault(phdl);
		overlay_plugin_walk(overlay_propinfo_plugin_cb, phdl);
		break;
	case OVERLAY_DEV_P_VARPDID:
		overlay_prop_set_prot(phdl, OVERLAY_PROP_PERM_READ);
		overlay_prop_set_type(phdl, OVERLAY_PROP_T_UINT);
		overlay_prop_set_nodefault(phdl);
		break;
	default:
		overlay_hold_rele(odd);
		mac_perim_exit(mph);
		return (ENOENT);
	}

	overlay_hold_rele(odd);
	mac_perim_exit(mph);
	return (0);
}

static int
overlay_i_getprop(void *karg, intptr_t arg, int mode, cred_t *cred,
    int *rvalp)
{
	int ret;
	overlay_dev_t *odd;
	mac_perim_handle_t mph;
	overlay_ioc_prop_t *oip = karg;
	uint_t propid, mtu;

	odd = overlay_hold_by_dlid(oip->oip_linkid);
	if (odd == NULL)
		return (ENOENT);

	mac_perim_enter_by_mh(odd->odd_mh, &mph);
	oip->oip_size = OVERLAY_PROP_SIZEMAX;
	oip->oip_name[OVERLAY_PROP_NAMELEN-1] = '\0';
	if (oip->oip_id == -1) {
		int i;

		for (i = 0; i < OVERLAY_DEV_NPROPS; i++) {
			if (strcmp(overlay_dev_props[i], oip->oip_name) == 0)
				break;
			if (i == OVERLAY_DEV_NPROPS) {
				ret = odd->odd_plugin->ovp_ops->ovpo_getprop(
				    odd->odd_pvoid, oip->oip_name,
				    oip->oip_value, &oip->oip_size);
				overlay_hold_rele(odd);
				mac_perim_exit(mph);
				return (ret);
			}
		}

		propid = i;
	} else if (oip->oip_id >= OVERLAY_DEV_NPROPS) {
		uint_t id = oip->oip_id - OVERLAY_DEV_NPROPS;

		if (id > odd->odd_plugin->ovp_nprops) {
			overlay_hold_rele(odd);
			mac_perim_exit(mph);
			return (EINVAL);
		}
		ret = odd->odd_plugin->ovp_ops->ovpo_getprop(odd->odd_pvoid,
		    odd->odd_plugin->ovp_props[id], oip->oip_value,
		    &oip->oip_size);
		overlay_hold_rele(odd);
		mac_perim_exit(mph);
		return (ret);
	} else if (oip->oip_id < -1) {
		overlay_hold_rele(odd);
		mac_perim_exit(mph);
		return (EINVAL);
	} else {
		ASSERT(oip->oip_id < OVERLAY_DEV_NPROPS);
		ASSERT(oip->oip_id >= 0);
		propid = oip->oip_id;
	}

	ret = 0;
	switch (propid) {
	case OVERLAY_DEV_P_MTU:
		/*
		 * The MTU is always set and retrieved through MAC, to allow for
		 * MAC to do whatever it wants, as really that property belongs
		 * to MAC. This is important for things where vnics have hold on
		 * the MTU.
		 */
		mac_sdu_get(odd->odd_mh, NULL, &mtu);
		bcopy(&mtu, oip->oip_value, sizeof (uint_t));
		oip->oip_size = sizeof (uint_t);
		break;
	case OVERLAY_DEV_P_VNETID:
		/*
		 * While it's read-only while inside of a mux, we're not in a
		 * context that can guarantee that. Therefore we always grab the
		 * overlay_dev_t's odd_lock.
		 */
		mutex_enter(&odd->odd_lock);
		bcopy(&odd->odd_vid, oip->oip_value, sizeof (uint64_t));
		mutex_exit(&odd->odd_lock);
		oip->oip_size = sizeof (uint64_t);
		break;
	case OVERLAY_DEV_P_ENCAP:
		oip->oip_size = strlcpy((char *)oip->oip_value,
		    odd->odd_plugin->ovp_name, oip->oip_size);
		break;
	case OVERLAY_DEV_P_VARPDID:
		mutex_enter(&odd->odd_lock);
		if (odd->odd_flags & OVERLAY_F_VARPD) {
			const uint64_t val = odd->odd_target->ott_id;
			bcopy(&val, oip->oip_value, sizeof (uint64_t));
			oip->oip_size = sizeof (uint64_t);
		} else {
			oip->oip_size = 0;
		}
		mutex_exit(&odd->odd_lock);
		break;
	default:
		ret = ENOENT;
	}

	overlay_hold_rele(odd);
	mac_perim_exit(mph);
	return (ret);
}

static void
overlay_setprop_vnetid(overlay_dev_t *odd, uint64_t vnetid)
{
	mutex_enter(&odd->odd_lock);

	/* Simple case, not active */
	if (!(odd->odd_flags & OVERLAY_F_IN_MUX)) {
		odd->odd_vid = vnetid;
		mutex_exit(&odd->odd_lock);
		return;
	}

	/*
	 * In the hard case, we need to set the drop flag, quiesce I/O and then
	 * we can go ahead and do everything.
	 */
	odd->odd_flags |= OVERLAY_F_MDDROP;
	overlay_io_wait(odd, OVERLAY_F_IOMASK);
	mutex_exit(&odd->odd_lock);

	overlay_mux_remove_dev(odd->odd_mux, odd);
	mutex_enter(&odd->odd_lock);
	odd->odd_vid = vnetid;
	mutex_exit(&odd->odd_lock);
	overlay_mux_add_dev(odd->odd_mux, odd);

	mutex_enter(&odd->odd_lock);
	ASSERT(odd->odd_flags & OVERLAY_F_IN_MUX);
	odd->odd_flags &= ~OVERLAY_F_IN_MUX;
	mutex_exit(&odd->odd_lock);
}

static int
overlay_i_setprop(void *karg, intptr_t arg, int mode, cred_t *cred,
    int *rvalp)
{
	int ret;
	overlay_dev_t *odd;
	overlay_ioc_prop_t *oip = karg;
	uint_t propid = UINT_MAX;
	mac_perim_handle_t mph;
	uint64_t maxid, *vidp;

	if (oip->oip_size > OVERLAY_PROP_SIZEMAX)
		return (EINVAL);

	odd = overlay_hold_by_dlid(oip->oip_linkid);
	if (odd == NULL)
		return (ENOENT);

	oip->oip_name[OVERLAY_PROP_NAMELEN-1] = '\0';
	mac_perim_enter_by_mh(odd->odd_mh, &mph);
	mutex_enter(&odd->odd_lock);
	if (odd->odd_flags & OVERLAY_F_ACTIVATED) {
		mac_perim_exit(mph);
		mutex_exit(&odd->odd_lock);
		return (ENOTSUP);
	}
	mutex_exit(&odd->odd_lock);
	if (oip->oip_id == -1) {
		int i;

		for (i = 0; i < OVERLAY_DEV_NPROPS; i++) {
			if (strcmp(overlay_dev_props[i], oip->oip_name) == 0)
				break;
			if (i == OVERLAY_DEV_NPROPS) {
				ret = odd->odd_plugin->ovp_ops->ovpo_setprop(
				    odd->odd_pvoid, oip->oip_name,
				    oip->oip_value, oip->oip_size);
				overlay_hold_rele(odd);
				mac_perim_exit(mph);
				return (ret);
			}
		}

		propid = i;
	} else if (oip->oip_id >= OVERLAY_DEV_NPROPS) {
		uint_t id = oip->oip_id - OVERLAY_DEV_NPROPS;

		if (id > odd->odd_plugin->ovp_nprops) {
			mac_perim_exit(mph);
			overlay_hold_rele(odd);
			return (EINVAL);
		}
		ret = odd->odd_plugin->ovp_ops->ovpo_setprop(odd->odd_pvoid,
		    odd->odd_plugin->ovp_props[id], oip->oip_value,
		    oip->oip_size);
		mac_perim_exit(mph);
		overlay_hold_rele(odd);
		return (ret);
	} else if (oip->oip_id < -1) {
		mac_perim_exit(mph);
		overlay_hold_rele(odd);
		return (EINVAL);
	} else {
		ASSERT(oip->oip_id < OVERLAY_DEV_NPROPS);
		ASSERT(oip->oip_id >= 0);
		propid = oip->oip_id;
	}

	ret = 0;
	switch (propid) {
	case OVERLAY_DEV_P_MTU:
		ret = mac_set_prop(odd->odd_mh, MAC_PROP_MTU, "mtu",
		    oip->oip_value, oip->oip_size);
		break;
	case OVERLAY_DEV_P_VNETID:
		if (oip->oip_size != sizeof (uint64_t)) {
			ret = EINVAL;
			break;
		}
		vidp = (uint64_t *)oip->oip_value;
		ASSERT(odd->odd_plugin->ovp_id_size <= 8);
		maxid = UINT64_MAX;
		if (odd->odd_plugin->ovp_id_size != 8)
			maxid = (1ULL << (odd->odd_plugin->ovp_id_size * 8)) -
			    1ULL;
		if (*vidp >= maxid) {
			ret = EINVAL;
			break;
		}
		overlay_setprop_vnetid(odd, *vidp);
		break;
	case OVERLAY_DEV_P_ENCAP:
	case OVERLAY_DEV_P_VARPDID:
		ret = EPERM;
		break;
	default:
		ret = ENOENT;
	}

	mac_perim_exit(mph);
	overlay_hold_rele(odd);
	return (ret);
}

static int
overlay_i_status(void *karg, intptr_t arg, int mode, cred_t *cred,
    int *rvalp)
{
	overlay_dev_t *odd;
	overlay_ioc_status_t *os = karg;

	odd = overlay_hold_by_dlid(os->ois_linkid);
	if (odd == NULL)
		return (ENOENT);

	mutex_enter(&odd->odd_lock);
	if ((odd->odd_flags & OVERLAY_F_DEGRADED) != 0) {
		os->ois_status = OVERLAY_I_DEGRADED;
		if (odd->odd_fmamsg != NULL) {
			(void) strlcpy(os->ois_message, odd->odd_fmamsg,
			    OVERLAY_STATUS_BUFLEN);
		} else {
			os->ois_message[0] = '\0';
		}

	} else {
		os->ois_status = OVERLAY_I_OK;
		os->ois_message[0] = '\0';
	}
	mutex_exit(&odd->odd_lock);
	overlay_hold_rele(odd);

	return (0);
}

static dld_ioc_info_t overlay_ioc_list[] = {
	{ OVERLAY_IOC_CREATE, DLDCOPYIN, sizeof (overlay_ioc_create_t),
		overlay_i_create, secpolicy_dl_config },
	{ OVERLAY_IOC_ACTIVATE, DLDCOPYIN, sizeof (overlay_ioc_activate_t),
		overlay_i_activate, secpolicy_dl_config },
	{ OVERLAY_IOC_DELETE, DLDCOPYIN, sizeof (overlay_ioc_delete_t),
		overlay_i_delete, secpolicy_dl_config },
	{ OVERLAY_IOC_PROPINFO, DLDCOPYIN | DLDCOPYOUT,
		sizeof (overlay_ioc_propinfo_t), overlay_i_propinfo,
		secpolicy_dl_config },
	{ OVERLAY_IOC_GETPROP, DLDCOPYIN | DLDCOPYOUT,
		sizeof (overlay_ioc_prop_t), overlay_i_getprop,
		secpolicy_dl_config },
	{ OVERLAY_IOC_SETPROP, DLDCOPYIN,
		sizeof (overlay_ioc_prop_t), overlay_i_setprop,
		secpolicy_dl_config },
	{ OVERLAY_IOC_NPROPS, DLDCOPYIN | DLDCOPYOUT,
		sizeof (overlay_ioc_nprops_t), overlay_i_nprops,
		secpolicy_dl_config },
	{ OVERLAY_IOC_STATUS, DLDCOPYIN | DLDCOPYOUT,
		sizeof (overlay_ioc_status_t), overlay_i_status,
		NULL }
};

static int
overlay_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	int fmcap = DDI_FM_EREPORT_CAPABLE;
	if (cmd != DDI_ATTACH)
		return (DDI_FAILURE);

	if (overlay_dip != NULL || ddi_get_instance(dip) != 0)
		return (DDI_FAILURE);

	/*
	 * XXX The nexus driver doesn't actually support DDI_FM_REPORT, that's
	 * unfortunate.
	 */
	ddi_fm_init(dip, &fmcap, NULL);
	if (fmcap != DDI_FM_EREPORT_CAPABLE)
		cmn_err(CE_WARN, "XXX didn't get DDI_FM_EREPORT");

	if (ddi_create_minor_node(dip, OVERLAY_CTL, S_IFCHR,
	    ddi_get_instance(dip), DDI_PSEUDO, 0) == DDI_FAILURE)
		return (DDI_FAILURE);

	if (dld_ioc_register(OVERLAY_IOC, overlay_ioc_list,
	    DLDIOCCNT(overlay_ioc_list)) != 0) {
		ddi_remove_minor_node(dip, OVERLAY_CTL);
		return (DDI_FAILURE);
	}

	overlay_dip = dip;
	return (DDI_SUCCESS);
}

static int
overlay_getinfo(dev_info_t *dip, ddi_info_cmd_t cmd, void *arg, void **resp)
{
	int error;

	switch (cmd) {
	case DDI_INFO_DEVT2DEVINFO:
		*resp = (void *)overlay_dip;
		error = DDI_SUCCESS;
		break;
	case DDI_INFO_DEVT2INSTANCE:
		*resp = (void *)0;
		error = DDI_SUCCESS;
		break;
	default:
		error = DDI_FAILURE;
		break;
	}

	return (error);
}

static int
overlay_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	if (cmd != DDI_DETACH)
		return (DDI_FAILURE);

	mutex_enter(&overlay_dev_lock);
	if (!list_is_empty(&overlay_dev_list) || overlay_target_busy()) {
		mutex_exit(&overlay_dev_lock);
		return (EBUSY);
	}
	mutex_exit(&overlay_dev_lock);


	dld_ioc_unregister(VNIC_IOC);
	ddi_remove_minor_node(dip, OVERLAY_CTL);
	ddi_fm_fini(dip);
	overlay_dip = NULL;
	return (DDI_SUCCESS);
}

static struct cb_ops overlay_cbops = {
	overlay_target_open,	/* cb_open */
	overlay_target_close,	/* cb_close */
	nodev,			/* cb_strategy */
	nodev,			/* cb_print */
	nodev,			/* cb_dump */
	nodev,			/* cb_read */
	nodev,			/* cb_write */
	overlay_target_ioctl,	/* cb_ioctl */
	nodev,			/* cb_devmap */
	nodev,			/* cb_mmap */
	nodev,			/* cb_segmap */
	nochpoll,		/* cb_chpoll */
	ddi_prop_op,		/* cb_prop_op */
	NULL,			/* cb_stream */
	D_MP,			/* cb_flag */
	CB_REV,			/* cb_rev */
	nodev,			/* cb_aread */
	nodev,			/* cb_awrite */
};

static struct dev_ops overlay_dev_ops = {
	DEVO_REV,		/* devo_rev */
	0,			/* devo_refcnt */
	overlay_getinfo,	/* devo_getinfo */
	nulldev,		/* devo_identify */
	nulldev,		/* devo_probe */
	overlay_attach,		/* devo_attach */
	overlay_detach,		/* devo_detach */
	nulldev,		/* devo_reset */
	&overlay_cbops,		/* devo_cb_ops */
	NULL,			/* devo_bus_ops */
	NULL,			/* devo_power */
	ddi_quiesce_not_supported	/* devo_quiesce */
};

static struct modldrv overlay_modldrv = {
	&mod_driverops,
	"Overlay Network Driver",
	&overlay_dev_ops
};

static struct modlinkage overlay_linkage = {
	MODREV_1,
	&overlay_modldrv
};

static int
overlay_init(void)
{
	mutex_init(&overlay_dev_lock, NULL, MUTEX_DRIVER, NULL);
	list_create(&overlay_dev_list, sizeof (overlay_dev_t),
	    offsetof(overlay_dev_t, odd_link));
	overlay_mux_init();
	overlay_plugin_init();
	overlay_target_init();

	return (DDI_SUCCESS);
}

static void
overlay_fini(void)
{
	overlay_target_fini();
	overlay_plugin_fini();
	overlay_mux_fini();
	mutex_destroy(&overlay_dev_lock);
	list_destroy(&overlay_dev_list);
}

int
_init(void)
{
	int err;

	if ((err = overlay_init()) != DDI_SUCCESS)
		return (err);

	mac_init_ops(NULL, "overlay");
	err = mod_install(&overlay_linkage);
	if (err != DDI_SUCCESS) {
		overlay_fini();
		return (err);
	}

	return (0);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&overlay_linkage, modinfop));
}

int
_fini(void)
{
	int err;

	err = mod_remove(&overlay_linkage);
	if (err != 0)
		return (err);

	overlay_fini();
	return (0);
}
