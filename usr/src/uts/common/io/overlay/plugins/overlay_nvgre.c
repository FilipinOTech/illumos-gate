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
 * NVGRE encapsulation module
 */

#include <sys/overlay_plugin.h>
#include <sys/modctl.h>
#include <sys/errno.h>
#include <sys/nvgre.h>
#include <sys/byteorder.h>
#include <inet/ip.h>

static const char *nvgre_ident = "nvgre";
/* XXX this should probably be global across all plugin-modules */
static char *nvgre_defip = "::ffff:0.0.0.0";

/* XXX We're locking here, should the higher level serialize this for us? */
typedef struct nvgre {
	kmutex_t nvg_lock;
	struct in6_addr nvg_laddr;
} nvgre_t;

static const char *nvgre_props[] = {
	"nvgre/listen_ip",
	NULL
};

static int
nvgre_o_init(overlay_handle_t oh, void **outp)
{
	nvgre_t *nvg;

	nvg = kmem_alloc(sizeof (nvgre_t), KM_SLEEP);
	mutex_init(&nvg->nvg_lock, NULL, MUTEX_DRIVER, NULL);
	/* XXX Assert return? */
	(void) inet_pton(AF_INET6, nvgre_defip, &nvg->nvg_laddr);

	return (0);
}

static void
nvgre_o_fini(void *arg)
{
	nvgre_t *nvg = arg;
	mutex_destroy(&nvg->nvg_lock);
	kmem_free(arg, sizeof (nvgre_t));
}

static int
nvgre_o_socket(void *arg, int *dp, int *fp, int *pp, struct sockaddr *addr,
    socklen_t *slenp)
{
	nvgre_t *nvg = arg;
	struct sockaddr_in6 *in = (struct sockaddr_in6 *)addr;

	*dp = AF_INET6;
	*fp = SOCK_RAW;
	*pp = 0x2f;	/* XXX Move into a IPPPROTO_* def */

	bzero(in, sizeof (struct sockaddr_in6));
	in->sin6_family = AF_INET6;

	mutex_enter(&nvg->nvg_lock);
	in->sin6_addr = nvg->nvg_laddr;
	mutex_exit(&nvg->nvg_lock);
	in->sin6_port = 0;
	*slenp = sizeof (struct sockaddr_in6);

	return (0);
}

/* XXX Should we keep track of kstats here? */

int
nvgre_o_encap(void *arg, mblk_t *mp, ovep_encap_info_t *einfop,
    mblk_t **outp)
{
	mblk_t *op;
	nvgre_hdr_t *hp;
	uint32_t id;

	ASSERT(einfop->ovdi_id < 1<<24);
	op = allocb(NVGRE_HDR_LEN, 0);
	if (op == NULL)
		return (ENOMEM);

	hp = (nvgre_hdr_t *)op->b_rptr;
	hp->nvgre_flags = htons(NVGRE_FLAG_VALUE);
	hp->nvgre_prot = htons(NVGRE_PROTOCOL);
	id = (uint32_t)einfop->ovdi_id << NVGRE_ID_SHIFT;
	id |= einfop->ovdi_hash & NVGRE_FLOW_MASK;
	hp->nvgre_id = htonl(id);
	op->b_wptr += NVGRE_HDR_LEN;
	*outp = op;

	return (0);
}

int
nvgre_o_decap(void *arg, mblk_t *mp, ovep_encap_info_t *dinfop)
{
	uint32_t id;
	nvgre_hdr_t *hp;

	hp = (nvgre_hdr_t *)mp->b_rptr;
	if ((ntohs(hp->nvgre_flags) & NVGRE_FLAG_MASK) != NVGRE_FLAG_VALUE)
		return (EINVAL);

	if (ntohs(hp->nvgre_prot) != NVGRE_PROTOCOL)
		return (EINVAL);

	id = ntohl(hp->nvgre_id);
	dinfop->ovdi_id = (id & NVGRE_ID_MASK) >> NVGRE_ID_SHIFT;
	dinfop->ovdi_hdr_size = NVGRE_HDR_LEN;
	/* XXX I'm not really sure why we'd want to save the flow */
	dinfop->ovdi_hash = id & NVGRE_FLOW_MASK;
	/* XXX These fields may not be necessary */
	dinfop->ovdi_encap_type = -1;
	dinfop->ovdi_vlan = -1;

	return (0);
}

static struct overlay_plugin_ops nvgre_o_ops = {
	0,
	nvgre_o_init,
	nvgre_o_fini,
	nvgre_o_encap,
	nvgre_o_decap,
	nvgre_o_socket
};

static struct modlmisc nvgre_modlmisc = {
	&mod_miscops,
	"NVGRE encap plugin"
};

static struct modlinkage nvgre_modlinkage = {
	MODREV_1,
	&nvgre_modlmisc
};

int
_init(void)
{
	int err;
	overlay_plugin_register_t *ovrp;

	ovrp = overlay_plugin_alloc(OVEP_VERSION);
	if (ovrp == NULL)
		return (ENOTSUP);
	ovrp->ovep_name = nvgre_ident;
	ovrp->ovep_ops = &nvgre_o_ops;
	ovrp->ovep_id_size = NVGRE_ID_LEN;
	ovrp->ovep_flags = 0;
	ovrp->ovep_hdr_min = NVGRE_HDR_LEN;
	ovrp->ovep_hdr_max = NVGRE_HDR_LEN;
	ovrp->ovep_dest = OVERLAY_PLUGIN_D_IP;
	ovrp->ovep_props = nvgre_props;

	if ((err = overlay_plugin_register(ovrp)) == 0) {
		if ((err = mod_install(&nvgre_modlinkage)) != 0) {
			(void) overlay_plugin_unregister(nvgre_ident);
		}
	}

	overlay_plugin_free(ovrp);
	return (err);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&nvgre_modlinkage, modinfop));
}

int
_fini(void)
{
	int err;

	if ((err = overlay_plugin_unregister(nvgre_ident)) != 0)
		return (err);

	return (mod_remove(&nvgre_modlinkage));
}
