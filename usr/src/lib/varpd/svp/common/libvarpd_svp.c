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
 * Copyright (c) 2014, Joyent, Inc.
 */

/*
 * This plugin implements the SDC VXLAN Protocol (SVP).
 *
 * XXX Expand on everything.
 */

#include <umem.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <libnvpair.h>
#include <strings.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include <libvarpd_provider.h>
#include "libvarpd_svp.h"

bunyan_logger_t *svp_bunyan;
static int svp_defport = 1296;
static int svp_defuport = 1339;
static umem_cache_t *svp_lookup_cache;

typedef enum svp_lookup_type {
	SVP_L_UNKNOWN	= 0x0,
	SVP_L_VL2	= 0x1,
	SVP_L_VL3	= 0x2
} svp_lookup_type_t;

typedef struct svp_lookup {
	int svl_type;
	union {
		struct svl_lookup_vl2 {
			varpd_query_handle_t	*svl_handle;
			overlay_target_point_t	*svl_point;
		} svl_vl2;
		struct svl_lookup_vl3 {
			varpd_arp_handle_t	*svl_vah;
			uint8_t			*svl_out;
		} svl_vl3;
	} svl_u;
	svp_query_t				svl_query;
} svp_lookup_t;

static const char *varpd_svp_props[] = {
	"svp/host",
	"svp/port",
	"svp/underlay_ip",
	"svp/underlay_port"
};

int
svp_comparator(const void *l, const void *r)
{
	const svp_t *ls = l;
	const svp_t *rs = r;

	if (ls->svp_vid > rs->svp_vid)
		return (1);
	if (ls->svp_vid < rs->svp_vid)
		return (-1);
	return (0);
}

static void
svp_vl2_lookup_cb(svp_t *svp, svp_status_t status, const struct in6_addr *uip,
    const uint16_t uport, void *arg)
{
	svp_lookup_t *svl = arg;
	overlay_target_point_t *otp;

	assert(svp != NULL);
	assert(arg != NULL);

	if (status != SVP_S_OK) {
		libvarpd_plugin_query_reply(svl->svl_u.svl_vl2.svl_handle,
		    VARPD_LOOKUP_DROP);
		umem_cache_free(svp_lookup_cache, svl);
		return;
	}

	otp = svl->svl_u.svl_vl2.svl_point;
	bcopy(uip, &otp->otp_ip, sizeof (struct in6_addr));
	otp->otp_port = uport;
	libvarpd_plugin_query_reply(svl->svl_u.svl_vl2.svl_handle,
	    VARPD_LOOKUP_OK);
	umem_cache_free(svp_lookup_cache, svl);
}

static void
svp_vl3_lookup_cb(svp_t *svp, svp_status_t status, const uint8_t *vl2mac,
    const struct in6_addr *uip, const uint16_t uport, void *arg)
{
	overlay_target_point_t point;
	svp_lookup_t *svl = arg;

	assert(svp != NULL);
	assert(svl != NULL);

	if (status != SVP_S_OK) {
		libvarpd_plugin_arp_reply(svl->svl_u.svl_vl3.svl_vah,
		    VARPD_LOOKUP_DROP);
		umem_cache_free(svp_lookup_cache, svl);
		return;
	}

	/* Inject the L2 mapping before the L3 */
	bcopy(uip, &point.otp_ip, sizeof (struct in6_addr));
	point.otp_port = uport;
	libvarpd_inject_varp(svp->svp_hdl, vl2mac, &point);

	bcopy(vl2mac, svl->svl_u.svl_vl3.svl_out, ETHERADDRL);
	libvarpd_plugin_arp_reply(svl->svl_u.svl_vl3.svl_vah,
	    VARPD_LOOKUP_OK);
	umem_cache_free(svp_lookup_cache, svl);
}

static void
svp_vl2_invalidate_cb(svp_t *svp, const uint8_t *vl2mac)
{
	libvarpd_inject_varp(svp->svp_hdl, vl2mac, NULL);
}

static void
svp_vl3_inject_cb(svp_t *svp, const uint16_t vlan, const struct in6_addr *vl3ip,
    const uint8_t *vl2mac, const uint8_t *targmac)
{
	struct in_addr v4;

	if (IN6_IS_ADDR_V4MAPPED(vl3ip) == 0)
		libvarpd_panic("implement libvarpd_inject_ndp");
	IN6_V4MAPPED_TO_INADDR(vl3ip, &v4);
	libvarpd_inject_arp(svp->svp_hdl, vlan, vl2mac, &v4, targmac);
}

static void
svp_shootdown_cb(svp_t *svp, const uint8_t *vl2mac, const struct in6_addr *uip,
    const uint16_t uport)
{
	/*
	 * XXX We should probably do a conditional invlaidation here.
	 */
	libvarpd_inject_varp(svp->svp_hdl, vl2mac, NULL);
}

static svp_cb_t svp_defops = {
	svp_vl2_lookup_cb,
	svp_vl3_lookup_cb,
	svp_vl2_invalidate_cb,
	svp_vl3_inject_cb,
	svp_shootdown_cb
};

static boolean_t
varpd_svp_valid_dest(overlay_plugin_dest_t dest)
{
	if (dest != (OVERLAY_PLUGIN_D_IP | OVERLAY_PLUGIN_D_PORT))
		return (B_FALSE);

	return (B_TRUE);
}

static int
varpd_svp_create(varpd_provider_handle_t *hdl, void **outp,
    overlay_plugin_dest_t dest)
{
	int ret;
	svp_t *svp;

	if (varpd_svp_valid_dest(dest) == B_FALSE)
		return (ENOTSUP);

	svp = umem_zalloc(sizeof (svp_t), UMEM_DEFAULT);
	if (svp == NULL)
		return (ENOMEM);

	if ((ret = mutex_init(&svp->svp_lock, USYNC_THREAD, NULL) != 0)) {
		umem_free(svp, sizeof (svp_t));
		return (ret);
	}

	svp->svp_port = svp_defport;
	svp->svp_uport = svp_defuport;
	svp->svp_cb = svp_defops;
	svp->svp_hdl = hdl;
	svp->svp_vid = libvarpd_plugin_vnetid(svp->svp_hdl);
	*outp = svp;
	return (0);
}

static int
varpd_svp_start(void *arg)
{
	int ret;
	svp_remote_t *srp;
	svp_t *svp = arg;

	mutex_lock(&svp->svp_lock);
	if (svp->svp_host == NULL || svp->svp_port == 0 ||
	    svp->svp_huip == B_FALSE || svp->svp_uport == 0) {
		mutex_unlock(&svp->svp_lock);
		return (EAGAIN);
	}
	mutex_unlock(&svp->svp_lock);

	if ((ret = svp_remote_find(svp->svp_host, svp->svp_port, &srp)) != 0)
		return (ret);

	if ((ret = svp_remote_attach(srp, svp)) != 0) {
		svp_remote_release(srp);
		return (ret);
	}

	return (0);
}

static void
varpd_svp_stop(void *arg)
{
	svp_t *svp = arg;

	svp_remote_detach(svp);
}

static void
varpd_svp_destroy(void *arg)
{
	svp_t *svp = arg;

	if (svp->svp_host != NULL)
		umem_free(svp->svp_host, strlen(svp->svp_host) + 1);

	if (mutex_destroy(&svp->svp_lock) != 0)
		libvarpd_panic("failed to destroy svp_t`svp_lock");

	umem_free(svp, sizeof (svp_t));
}

static void
varpd_svp_lookup(void *arg, varpd_query_handle_t *vqh,
    const overlay_targ_lookup_t *otl, overlay_target_point_t *otp)
{
	svp_lookup_t *slp;
	svp_t *svp = arg;

	/*
	 * Check if this is something that we need to proxy, eg. arp or ndp.
	 */
	if (otl->otl_sap == ETHERTYPE_ARP) {
		libvarpd_plugin_proxy_arp(svp->svp_hdl, vqh, otl);
		return;
	}

	if (otl->otl_sap == ETHERTYPE_IPV6 &&
	    otl->otl_dstaddr[0] == 0x33 &&
	    otl->otl_dstaddr[1] == 0x33) {
		libvarpd_plugin_proxy_ndp(svp->svp_hdl, vqh, otl);
	}

	/* XXX CACHES */

	/*
	 * If we have a failure to allocate memory for this, that's not good.
	 * However, telling the kernel to just drop this packet is much better
	 * than the alternative at this moment. At least we'll try again and we
	 * may have something more available to us in a little bit.
	 *
	 * TODO We need to have observability around this case.
	 */
	slp = umem_cache_alloc(svp_lookup_cache, UMEM_DEFAULT);
	if (slp == NULL) {
		libvarpd_plugin_query_reply(vqh, VARPD_LOOKUP_DROP);
		return;
	}

	slp->svl_type = SVP_L_VL2;
	slp->svl_u.svl_vl2.svl_handle = vqh;
	slp->svl_u.svl_vl2.svl_point = otp;

	svp_remote_vl2_lookup(svp, &slp->svl_query, otl->otl_dstaddr, slp);
}

static int
varpd_svp_nprops(void *arg, uint_t *nprops)
{
	*nprops = sizeof (varpd_svp_props) / sizeof (char *);
	return (0);
}

static int
varpd_svp_propinfo(void *arg, uint_t propid, varpd_prop_handle_t *vph)
{
	switch (propid) {
	case 0:
		/* svp/host */
		libvarpd_prop_set_name(vph, varpd_svp_props[0]);
		libvarpd_prop_set_prot(vph, OVERLAY_PROP_PERM_RRW);
		libvarpd_prop_set_type(vph, OVERLAY_PROP_T_STRING);
		libvarpd_prop_set_nodefault(vph);
		break;
	case 1:
		/* svp/port */
		libvarpd_prop_set_name(vph, varpd_svp_props[1]);
		libvarpd_prop_set_prot(vph, OVERLAY_PROP_PERM_RRW);
		libvarpd_prop_set_type(vph, OVERLAY_PROP_T_UINT);
		libvarpd_prop_set_default(vph, &svp_defport,
		    sizeof (svp_defport));
		libvarpd_prop_set_range_uint32(vph, 1, UINT16_MAX);
		break;
	case 2:
		/* svp/underlay_ip */
		libvarpd_prop_set_name(vph, varpd_svp_props[2]);
		libvarpd_prop_set_prot(vph, OVERLAY_PROP_PERM_RRW);
		libvarpd_prop_set_type(vph, OVERLAY_PROP_T_IP);
		libvarpd_prop_set_nodefault(vph);
		break;
	case 3:
		/* svp/underlay_port */
		libvarpd_prop_set_name(vph, varpd_svp_props[3]);
		libvarpd_prop_set_prot(vph, OVERLAY_PROP_PERM_RRW);
		libvarpd_prop_set_type(vph, OVERLAY_PROP_T_UINT);
		libvarpd_prop_set_default(vph, &svp_defuport,
		    sizeof (svp_defuport));
		libvarpd_prop_set_range_uint32(vph, 1, UINT16_MAX);
		break;
	default:
		return (EINVAL);
	}
	return (0);
}

static int
varpd_svp_getprop(void *arg, const char *pname, void *buf, uint32_t *sizep)
{
	svp_t *svp = arg;

	/* svp/host */
	if (strcmp(pname, varpd_svp_props[0]) == 0) {
		size_t len;

		mutex_lock(&svp->svp_lock);
		if (svp->svp_host == NULL) {
			*sizep = 0;
		} else {
			len = strlen(svp->svp_host) + 1;
			if (*sizep < len) {
				mutex_unlock(&svp->svp_lock);
				return (EOVERFLOW);
			}
			*sizep = len;
			(void) strlcpy(buf, svp->svp_host, *sizep);
		}
		mutex_unlock(&svp->svp_lock);
		return (0);
	}

	/* svp/port */
	if (strcmp(pname, varpd_svp_props[1]) == 0) {
		uint64_t val;

		if (*sizep < sizeof (uint64_t))
			return (EOVERFLOW);

		mutex_lock(&svp->svp_lock);
		if (svp->svp_port == 0) {
			*sizep = 0;
		} else {
			val = svp->svp_port;
			bcopy(&val, buf, sizeof (uint64_t));
			*sizep = sizeof (uint64_t);
		}

		mutex_unlock(&svp->svp_lock);
		return (0);
	}

	/* svp/underlay_ip */
	if (strcmp(pname, varpd_svp_props[2]) == 0) {
		if (*sizep > sizeof (struct in6_addr))
			return (EOVERFLOW);
		mutex_lock(&svp->svp_lock);
		if (svp->svp_huip == B_FALSE) {
			*sizep = 0;
		} else {
			bcopy(&svp->svp_uip, buf, sizeof (struct in6_addr));
			*sizep = sizeof (struct in6_addr);
		}
		return (0);
	}

	/* svp/underlay_port */
	if (strcmp(pname, varpd_svp_props[3]) == 0) {
		uint64_t val;

		if (*sizep < sizeof (uint64_t))
			return (EOVERFLOW);

		mutex_lock(&svp->svp_lock);
		if (svp->svp_uport == 0) {
			*sizep = 0;
		} else {
			val = svp->svp_uport;
			bcopy(&val, buf, sizeof (uint64_t));
			*sizep = sizeof (uint64_t);
		}

		mutex_unlock(&svp->svp_lock);
		return (0);
	}

	return (EINVAL);
}

static int
varpd_svp_setprop(void *arg, const char *pname, const void *buf,
    const uint32_t size)
{
	svp_t *svp = arg;

	/* svp/host */
	if (strcmp(pname, varpd_svp_props[0]) == 0) {
		char *dup;
		/* XXX Validate hostname characters, maybe grab a C locale */
		dup = umem_alloc(size, UMEM_DEFAULT);
		(void) strlcpy(dup, buf, size);
		if (dup == NULL)
			return (ENOMEM);
		mutex_lock(&svp->svp_lock);
		if (svp->svp_host != NULL)
			umem_free(svp->svp_host, strlen(svp->svp_host) + 1);
		svp->svp_host = dup;
		mutex_unlock(&svp->svp_lock);
		return (0);
	}

	/* svp/port */
	if (strcmp(pname, varpd_svp_props[1]) == 0) {
		const uint64_t *valp = buf;
		if (size < sizeof (uint64_t))
			return (EOVERFLOW);

		if (*valp == 0 || *valp > UINT16_MAX)
			return (EINVAL);

		mutex_lock(&svp->svp_lock);
		svp->svp_port = (uint16_t)*valp;
		mutex_unlock(&svp->svp_lock);
		return (0);
	}

	/* svp/underlay_ip */
	if (strcmp(pname, varpd_svp_props[2]) == 0) {
		const struct in6_addr *ipv6 = buf;

		if (size < sizeof (struct in6_addr))
			return (EOVERFLOW);

		/*
		 * XXX Is anything else disallowed?
		 */
		if (IN6_IS_ADDR_V4COMPAT(ipv6))
			return (EINVAL);
		mutex_lock(&svp->svp_lock);
		bcopy(buf, &svp->svp_uip, sizeof (struct in6_addr));
		svp->svp_huip = B_TRUE;
		mutex_unlock(&svp->svp_lock);
		return (0);
	}

	/* svp/underlay_port */
	if (strcmp(pname, varpd_svp_props[3]) == 0) {
		const uint64_t *valp = buf;
		if (size < sizeof (uint64_t))
			return (EOVERFLOW);

		if (*valp == 0 || *valp > UINT16_MAX)
			return (EINVAL);

		mutex_lock(&svp->svp_lock);
		svp->svp_uport = (uint16_t)*valp;
		mutex_unlock(&svp->svp_lock);

		return (0);
	}

	return (EINVAL);
}

static int
varpd_svp_save(void *arg, nvlist_t *nvp)
{
	int ret;
	svp_t *svp = arg;

	mutex_lock(&svp->svp_lock);
	if (svp->svp_host != NULL) {
		if ((ret = nvlist_add_string(nvp, varpd_svp_props[0],
		    svp->svp_host)) != 0) {
			mutex_unlock(&svp->svp_lock);
			return (ret);
		}
	}

	if (svp->svp_port != 0) {
		if ((ret = nvlist_add_uint16(nvp, varpd_svp_props[1],
		    svp->svp_port)) != 0) {
			mutex_unlock(&svp->svp_lock);
			return (ret);
		}
	}

	if (svp->svp_huip == B_TRUE) {
		char buf[INET6_ADDRSTRLEN];

		if (inet_ntop(AF_INET6, &svp->svp_uip, buf, sizeof (buf)) ==
		    NULL)
			libvarpd_panic("unexpected inet_ntop failure: %d",
			    errno);

		if ((ret = nvlist_add_string(nvp, varpd_svp_props[2],
		    buf)) != 0) {
			mutex_unlock(&svp->svp_lock);
			return (ret);
		}
	}

	if (svp->svp_uport != 0) {
		if ((ret = nvlist_add_uint16(nvp, varpd_svp_props[3],
		    svp->svp_uport)) != 0) {
			mutex_unlock(&svp->svp_lock);
			return (ret);
		}
	}

	mutex_unlock(&svp->svp_lock);
	return (0);
}

static int
varpd_svp_restore(nvlist_t *nvp, varpd_provider_handle_t *hdl,
    overlay_plugin_dest_t dest, void **outp)
{
	int ret;
	svp_t *svp;
	char *ipstr, *hstr;

	if (varpd_svp_valid_dest(dest) == B_FALSE)
		return (ENOTSUP);

	if ((ret = varpd_svp_create(hdl, (void **)&svp, dest)) != 0)
		return (ret);

	/* XXX Validate hostname */
	if ((ret = nvlist_lookup_string(nvp, varpd_svp_props[0],
	    &hstr)) != 0) {
		if (ret != ENOENT) {
			varpd_svp_destroy(svp);
			return (ret);
		}
		svp->svp_host = NULL;
	} else {
		size_t blen = strlen(hstr) + 1;
		svp->svp_host = umem_alloc(blen, UMEM_DEFAULT);
		(void) strlcpy(svp->svp_host, hstr, blen);
	}

	if ((ret = nvlist_lookup_uint16(nvp, varpd_svp_props[1],
	    &svp->svp_port)) != 0) {
		if (ret != ENOENT) {
			varpd_svp_destroy(svp);
			return (ret);
		}
		svp->svp_port = 0;
	}

	if ((ret = nvlist_lookup_string(nvp, varpd_svp_props[2],
	    &ipstr)) != 0) {
		if (ret != ENOENT) {
			varpd_svp_destroy(svp);
			return (ret);
		}
		svp->svp_huip = B_FALSE;
	} else {
		ret = inet_pton(AF_INET6, ipstr, &svp->svp_uip);
		if (ret == -1) {
			assert(errno == EAFNOSUPPORT);
			libvarpd_panic("unexpected inet_pton failure: %d",
			    errno);
		}

		if (ret == 0) {
			varpd_svp_destroy(svp);
			return (EINVAL);
		}
		svp->svp_huip = B_TRUE;
	}

	if ((ret = nvlist_lookup_uint16(nvp, varpd_svp_props[3],
	    &svp->svp_uport)) != 0) {
		if (ret != ENOENT) {
			varpd_svp_destroy(svp);
			return (ret);
		}
		svp->svp_uport = 0;
	}

	svp->svp_hdl = hdl;
	*outp = svp;
	return (0);
}

static void
varpd_svp_arp(void *arg, varpd_arp_handle_t *vah, int type,
    const struct sockaddr *sock, uint8_t *out)
{
	svp_t *svp = arg;
	svp_lookup_t *svl;

	if (type != VARPD_QTYPE_ETHERNET) {
		libvarpd_plugin_arp_reply(vah, VARPD_LOOKUP_DROP);
		return;
	}

	/* XXX CACHES */

	svl = umem_cache_alloc(svp_lookup_cache, UMEM_DEFAULT);
	if (svl == NULL) {
		libvarpd_plugin_arp_reply(vah, VARPD_LOOKUP_DROP);
		return;
	}

	svl->svl_type = SVP_L_VL3;
	svl->svl_u.svl_vl3.svl_vah = vah;
	svl->svl_u.svl_vl3.svl_out = out;
	svp_remote_vl3_lookup(svp, &svl->svl_query, sock, svl);
}

static const varpd_plugin_ops_t varpd_svp_ops = {
	0,
	varpd_svp_create,
	varpd_svp_start,
	varpd_svp_stop,
	varpd_svp_destroy,
	NULL,
	varpd_svp_lookup,
	varpd_svp_nprops,
	varpd_svp_propinfo,
	varpd_svp_getprop,
	varpd_svp_setprop,
	varpd_svp_save,
	varpd_svp_restore,
	varpd_svp_arp,
	NULL
};

static int
svp_bunyan_init(void)
{
	int ret;

	if ((ret = bunyan_init("svp", &svp_bunyan)) != 0)
		return (ret);
	ret = bunyan_stream_add(svp_bunyan, "stderr", BUNYAN_L_INFO,
	    bunyan_stream_fd, (void *)STDERR_FILENO);
	if (ret != 0)
		bunyan_fini(svp_bunyan);
	return (ret);
}

static void
svp_bunyan_fini(void)
{
	if (svp_bunyan != NULL)
		bunyan_fini(svp_bunyan);
}

#pragma init(varpd_svp_init)
static void
varpd_svp_init(void)
{
	int err;
	varpd_plugin_register_t *vpr;

	/* XXX Revisit and make sure we have proper clean up */
	if (svp_bunyan_init() != 0)
		return;

	if ((err == svp_host_init()) != 0) {
		svp_bunyan_fini();
		return;
	}

	/* XXX Communicate failure */
	svp_lookup_cache = umem_cache_create("svp_lookup",
	    sizeof (svp_lookup_t),  0, NULL, NULL, NULL, NULL, NULL, 0);
	if (svp_lookup_cache == NULL) {
		svp_bunyan_fini();
		return;
	}

	if ((err = svp_event_init()) != 0) {
		svp_bunyan_fini();
		umem_cache_destroy(svp_lookup_cache);
		return;
	}

	if ((err = svp_timer_init()) != 0) {
		svp_event_fini();
		umem_cache_destroy(svp_lookup_cache);
		svp_bunyan_fini();
		return;
	}

	if ((err = svp_remote_init()) != 0) {
		svp_event_fini();
		umem_cache_destroy(svp_lookup_cache);
		svp_bunyan_fini();
		return;
	}

	/* XXX Revisit failure semantics here */
	vpr = libvarpd_plugin_alloc(VARPD_CURRENT_VERSION, &err);
	if (vpr == NULL) {
		svp_remote_fini();
		svp_event_fini();
		umem_cache_destroy(svp_lookup_cache);
		return;
	}

	vpr->vpr_mode = OVERLAY_TARGET_DYNAMIC;
	vpr->vpr_name = "svp";
	vpr->vpr_ops = &varpd_svp_ops;

	(void) libvarpd_plugin_register(vpr);
	libvarpd_plugin_free(vpr);
}
