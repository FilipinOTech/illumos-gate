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
 * Copyright (c) 2014 Joyent, Inc.  All rights reserved.
 */

/*
 * VXLAN encapsulation module
 *
 *
 * The VXLAN header looks as follows in network byte order:
 *
 * |0        3| 4 |5                     31|
 * +----------+---+------------------------+
 * | Reserved | I | Reserved               |
 * +---------------------------------------+
 * | Virtual Network ID         | Reserved |
 * +----------------------------+----------+
 * |0                         23|24      31|
 * END CSTYLED
 *
 * All reserved values must be 0. The I bit must be 1. We call the top
 * word the VXLAN magic field for the time being. The second word is
 * definitely not the most friendly way to operate. Specifically, the ID
 * is a 24-bit big endian value, but we have to make sure not to use the
 * lower word.
 */

#include <sys/overlay_plugin.h>
#include <sys/modctl.h>
#include <sys/errno.h>
#include <sys/byteorder.h>
#include <sys/vxlan.h>
#include <inet/ip.h>
#include <netinet/in.h>

static const char *vxlan_ident = "vxlan";
static uint16_t vxlan_defport = IPPORT_VXLAN;

static const char *vxlan_props[] = {
	"vxlan/listen_ip",
	"vxlan/listen_port",
	NULL
};

/* XXX Should we do locking or let the higher level do it for us? */
typedef struct vxlan {
	kmutex_t vxl_lock;
	overlay_handle_t vxl_oh;
	uint16_t vxl_lport;
	boolean_t vxl_hladdr;
	struct in6_addr vxl_laddr;
} vxlan_t;

static int
vxlan_o_init(overlay_handle_t oh, void **outp)
{
	vxlan_t *vxl;

	vxl = kmem_alloc(sizeof (vxlan_t), KM_SLEEP);
	*outp = vxl;
	mutex_init(&vxl->vxl_lock, NULL, MUTEX_DRIVER, NULL);
	vxl->vxl_oh = oh;
	vxl->vxl_lport = vxlan_defport;
	vxl->vxl_hladdr = B_FALSE;

	return (0);
}

static void
vxlan_o_fini(void *arg)
{
	vxlan_t *vxl = arg;

	mutex_destroy(&vxl->vxl_lock);
	kmem_free(arg, sizeof (vxlan_t));
}

static int
vxlan_o_socket(void *arg, int *dp, int *fp, int *pp, struct sockaddr *addr,
    socklen_t *slenp)
{
	vxlan_t *vxl = arg;
	struct sockaddr_in6 *in = (struct sockaddr_in6 *)addr;

	*dp = AF_INET6;
	*fp = SOCK_DGRAM;
	*pp = 0;
	bzero(in, sizeof (struct sockaddr_in6));
	in->sin6_family = AF_INET6;

	mutex_enter(&vxl->vxl_lock);
	if (vxl->vxl_hladdr == B_FALSE) {
		mutex_exit(&vxl->vxl_lock);
		/* XXX This errno sucks and is now about trains */
		return (EINVAL);
	}
	in->sin6_port = htons(vxl->vxl_lport);
	in->sin6_addr = vxl->vxl_laddr;
	mutex_exit(&vxl->vxl_lock);
	*slenp = sizeof (struct sockaddr_in6);

	return (0);
}

/*
 * XXX Stats?
 */
static int
vxlan_o_encap(void *arg, mblk_t *mp, ovep_encap_info_t *einfop,
    mblk_t **outp)
{
	mblk_t *ob;
	vxlan_hdr_t *vxh;

	ASSERT(einfop->ovdi_id < (1 << 24));

	/*
	 * XXX We probably want a good way to cache and handle the allocation
	 * and destruction of these message blocks.
	 */
	ob = allocb(VXLAN_HDR_LEN, 0);
	if (ob == NULL)
		return (ENOMEM);



	vxh = (vxlan_hdr_t *)ob->b_rptr;
	vxh->vxlan_flags = ntohl(VXLAN_MAGIC);
	vxh->vxlan_id = htonl((uint32_t)einfop->ovdi_id << VXLAN_ID_SHIFT);
	ob->b_wptr += VXLAN_HDR_LEN;
	*outp = ob;

	return (0);
}

/* XXX Stats */
static int
vxlan_o_decap(void *arg, mblk_t *mp, ovep_encap_info_t *dinfop)
{
	vxlan_hdr_t *vxh;

	/* XXX This assumes that we have a pulled up block, which is false */
	vxh = (vxlan_hdr_t *)mp->b_rptr;
	if ((ntohl(vxh->vxlan_flags) & VXLAN_MAGIC) == 0)
		return (EINVAL);

	dinfop->ovdi_id = ntohl(vxh->vxlan_id) >> VXLAN_ID_SHIFT;
	dinfop->ovdi_hdr_size = VXLAN_HDR_LEN;
	/* XXX Probably don't need these fields in the long run */
	dinfop->ovdi_encap_type = -1;
	dinfop->ovdi_vlan = -1;

	return (0);
}

static int
vxlan_o_getprop(void *arg, const char *pr_name, void *buf, uint32_t *bufsize)
{
	vxlan_t *vxl = arg;

	/* vxlan/listen_ip */
	if (strcmp(pr_name, vxlan_props[0]) == 0) {
		if (*bufsize < sizeof (struct in6_addr))
			return (EOVERFLOW);

		mutex_enter(&vxl->vxl_lock);
		if (vxl->vxl_hladdr == B_FALSE) {
			*bufsize = 0;
		} else {
			bcopy(&vxl->vxl_laddr, buf, sizeof (struct in6_addr));
			*bufsize = sizeof (struct in6_addr);
		}
		mutex_exit(&vxl->vxl_lock);
		return (0);
	}

	/* vxlan/listen_port */
	if (strcmp(pr_name, vxlan_props[1]) == 0) {
		uint64_t val;
		if (*bufsize < sizeof (uint64_t))
			return (EOVERFLOW);

		mutex_enter(&vxl->vxl_lock);
		val = vxl->vxl_lport;
		bcopy(&val, buf, sizeof (uint64_t));
		*bufsize = sizeof (uint64_t);
		mutex_exit(&vxl->vxl_lock);
		return (0);
	}

	return (EINVAL);
}

static int
vxlan_o_setprop(void *arg, const char *pr_name, const void *buf,
    uint32_t bufsize)
{
	vxlan_t *vxl = arg;

	/* vxlan/listen_ip */
	if (strcmp(pr_name, vxlan_props[0]) == 0) {
		const struct in6_addr *ipv6 = buf;
		if (bufsize != sizeof (struct in6_addr))
			return (EINVAL);

		/*
		 * XXX What else should be disallowed?
		 */
		if (IN6_IS_ADDR_V4COMPAT(ipv6))
			return (EINVAL);

		mutex_enter(&vxl->vxl_lock);
		vxl->vxl_hladdr = B_TRUE;
		bcopy(ipv6, &vxl->vxl_laddr, sizeof (struct in6_addr));
		mutex_exit(&vxl->vxl_lock);

		return (0);
	}

	/* vxlan/listen_port */
	if (strcmp(pr_name, vxlan_props[1]) == 0) {
		const uint64_t *valp = buf;
		if (bufsize != 8)
			return (EINVAL);

		if (*valp == 0 || *valp > UINT16_MAX)
			return (EINVAL);

		mutex_enter(&vxl->vxl_lock);
		vxl->vxl_lport = *valp;
		mutex_exit(&vxl->vxl_lock);
		return (0);
	}
	return (EINVAL);
}

static int
vxlan_o_propinfo(const char *pr_name, overlay_prop_handle_t phdl)
{
	/* vxlan/listen_ip */
	if (strcmp(pr_name, vxlan_props[0]) == 0) {
		overlay_prop_set_name(phdl, vxlan_props[0]);
		overlay_prop_set_prot(phdl, OVERLAY_PROP_PERM_RRW);
		overlay_prop_set_type(phdl, OVERLAY_PROP_T_IP);
		overlay_prop_set_nodefault(phdl);
		return (0);
	}

	if (strcmp(pr_name, vxlan_props[1]) == 0) {
		overlay_prop_set_name(phdl, vxlan_props[1]);
		overlay_prop_set_prot(phdl, OVERLAY_PROP_PERM_RRW);
		overlay_prop_set_type(phdl, OVERLAY_PROP_T_UINT);
		overlay_prop_set_default(phdl, &vxlan_defport,
		    sizeof (vxlan_defport));
		overlay_prop_set_range_uint32(phdl, 1, UINT16_MAX);
		return (0);
	}

	return (EINVAL);
}

static struct overlay_plugin_ops vxlan_o_ops = {
	0,
	vxlan_o_init,
	vxlan_o_fini,
	vxlan_o_encap,
	vxlan_o_decap,
	vxlan_o_socket,
	vxlan_o_getprop,
	vxlan_o_setprop,
	vxlan_o_propinfo
};

static struct modlmisc vxlan_modlmisc = {
	&mod_miscops,
	"VXLAN encap plugin"
};

static struct modlinkage vxlan_modlinkage = {
	MODREV_1,
	&vxlan_modlmisc
};

int
_init(void)
{
	int err;
	overlay_plugin_register_t *ovrp;

	ovrp = overlay_plugin_alloc(OVEP_VERSION);
	if (ovrp == NULL)
		return (ENOTSUP);
	ovrp->ovep_name = vxlan_ident;
	ovrp->ovep_ops = &vxlan_o_ops;
	ovrp->ovep_id_size = VXLAN_ID_LEN;
	ovrp->ovep_flags = OVEP_F_VLAN_TAG;
	ovrp->ovep_hdr_min = VXLAN_HDR_LEN;
	ovrp->ovep_hdr_max = VXLAN_HDR_LEN;
	ovrp->ovep_dest = OVERLAY_PLUGIN_D_IP | OVERLAY_PLUGIN_D_PORT;
	ovrp->ovep_props = vxlan_props;

	if ((err = overlay_plugin_register(ovrp)) == 0) {
		if ((err = mod_install(&vxlan_modlinkage)) != 0) {
			(void) overlay_plugin_unregister(vxlan_ident);
		}
	}

	overlay_plugin_free(ovrp);
	return (err);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&vxlan_modlinkage, modinfop));
}

int
_fini(void)
{
	int err;

	if ((err = overlay_plugin_unregister(vxlan_ident)) != 0)
		return (err);

	return (mod_remove(&vxlan_modlinkage));
}
