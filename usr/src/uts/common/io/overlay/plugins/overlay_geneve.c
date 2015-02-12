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
 * Geneve encapsulation module
 */

#include <sys/overlay_plugin.h>
#include <sys/modctl.h>
#include <sys/errno.h>
#include <sys/geneve.h>

static const char *geneve_ident = "geneve";

static const char *geneve_props[] = {
	"geneve/listen_ip",
	"geneve/listen_port",
	NULL
};

static int
geneve_o_init(overlay_handle_t oh, void **outp)
{
	*outp = NULL;
	return (0);
}

static void
geneve_o_fini(void *arg)
{
}

int
geneve_o_encap(void *arg, mblk_t *mp, ovep_encap_info_t *einfop,
    mblk_t **outp)
{
	mblk_t *op;
	geneve_hdr_t *hp;

	ASSERT(einfop->ovdi_id < (1 << 24));

	op = allocb(GENEVE_HDR_MIN, 0);
	if (op == NULL)
		return (ENOMEM);

	hp = (geneve_hdr_t *)op->b_rptr;
	hp->geneve_flags = 0;
	hp->geneve_prot = htons(GENEVE_PROT_ETHERNET);
	hp->geneve_id = htonl(einfop->ovdi_id << GENEVE_ID_SHIFT);
	op->b_wptr += GENEVE_HDR_MIN;
	*outp = op;

	return (0);
}

int
geneve_o_decap(void *arg, mblk_t *mp, ovep_encap_info_t *dinfop)
{
	geneve_hdr_t *hp;
	uint16_t flags;
	uint8_t len;

	hp = (geneve_hdr_t *)mp->b_rptr;
	flags = ntohs(hp->geneve_flags);

	if ((flags & GENEVE_VERS_MASK) != GENEVE_VERSION)
		return (EINVAL);

	len = (flags & GENEVE_OPT_MASK) >> GENEVE_OPT_SHIFT;

	/*
	 * Today we have no notion of control messages, so we'll need to drop
	 * those. We also support no options. Therefore if the critical options
	 * flag has been turned on and the spec says we MAY drop it, we shall.
	 * The idea being that if this option is critical and we don't
	 * understand it, well, we shouldn't go forward.
	 */
	if ((flags & GENEVE_F_OAM) || (flags & GENEVE_F_COPT))
		return (EINVAL);

	/*
	 * We may some day support non-Ethernet encapsulations, but that day is
	 * not today.
	 */
	if (ntohs(hp->geneve_prot) != GENEVE_PROT_ETHERNET)
		return (EINVAL);

	dinfop->ovdi_id = ntohl(hp->geneve_id) >> GENEVE_ID_SHIFT;
	dinfop->ovdi_hdr_size = GENEVE_HDR_MIN + len;

	/* XXX Fields we should opt to ignore probably */
	dinfop->ovdi_vlan = -1;
	dinfop->ovdi_encap_type = GENEVE_PROT_ETHERNET;

	return (0);
}

static struct overlay_plugin_ops geneve_o_ops = {
	0,
	geneve_o_init,
	geneve_o_fini,
	geneve_o_encap,
	geneve_o_decap
};

static struct modlmisc geneve_modlmisc = {
	&mod_miscops,
	"Geneve encap plugin"
};

static struct modlinkage geneve_modlinkage = {
	MODREV_1,
	&geneve_modlmisc
};

int
_init(void)
{
	int err;
	overlay_plugin_register_t *ovrp;

	ovrp = overlay_plugin_alloc(OVEP_VERSION);
	if (ovrp == NULL)
		return (ENOTSUP);
	ovrp->ovep_name = geneve_ident;
	ovrp->ovep_ops = &geneve_o_ops;
	ovrp->ovep_id_size = GENEVE_ID_LEN;
	ovrp->ovep_flags = OVEP_F_VLAN_TAG;
	ovrp->ovep_hdr_min = GENEVE_HDR_MIN;
	ovrp->ovep_hdr_max = GENEVE_HDR_MAX;
	ovrp->ovep_dest = OVERLAY_PLUGIN_D_IP | OVERLAY_PLUGIN_D_PORT;
	ovrp->ovep_props = geneve_props;

	if ((err = overlay_plugin_register(ovrp)) == 0) {
		if ((err = mod_install(&geneve_modlinkage)) != 0) {
			(void) overlay_plugin_unregister(geneve_ident);
		}
	}

	overlay_plugin_free(ovrp);
	return (err);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&geneve_modlinkage, modinfop));
}

int
_fini(void)
{
	int err;

	if ((err = overlay_plugin_unregister(geneve_ident)) != 0)
		return (err);

	return (mod_remove(&geneve_modlinkage));
}
