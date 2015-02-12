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
 * Copyright (c) 2014 Joyent, Inc.
 */

#ifndef _LIBVARPD_PROVIDER_H
#define	_LIBVARPD_PROVIDER_H

/*
 * varpd provider interface
 *
 * This header file defines all the structures and functions that a given plugin
 * should register.
 */

#include <bunyan.h>
#include <libvarpd.h>
#include <libnvpair.h>
#include <sys/socket.h>
#include <sys/overlay_target.h>

#ifdef __cplusplus
extern "C" {
#endif

#define	VARPD_VERSION_ONE	1
#define	VARPD_CURRENT_VERSION	VARPD_VERSION_ONE

typedef struct __varpd_provier_handle varpd_provider_handle_t;
typedef struct __varpd_query_handle varpd_query_handle_t;
typedef struct __varpd_arp_handle varpd_arp_handle_t;
typedef struct __varpd_dhcp_handle varpd_dhcp_handle_t;

/*
 * Create a new instance of a plugin.
 */
typedef int (*varpd_plugin_create_f)(varpd_provider_handle_t *, void **,
    overlay_plugin_dest_t);

/*
 * Upon the return of this, the lookup function will be called.
 */
typedef int (*varpd_plugin_start_f)(void *);

/*
 * Upon the entry of this function, the lookup function will not be called.
 */
typedef void (*varpd_plugin_stop_f)(void *);

/*
 * Destroy an instance of a plugin.
 */
typedef void (*varpd_plugin_destroy_f)(void *);

/*
 * The varpd_plugin_default_f and varpd_plugin_lookup_f both look up
 * destinations and should have them written into the overlay_target_point_t.
 * The varpd_plugin_default_f should only be implemented for plugins which are
 * of type OVERLAY_TARGET_POINT, where as only the lookup function should be
 * implemented by plugins that are of type OVERLAY_TARGET_DYNAMIC.
 *
 * In both cases, the answer should be filled into the overlay_target_point_t.
 * In the case of the varpd_plugin_default_f, one of the VARPD_LOOKUP_* values
 * should be returned by the function.
 *
 * In the case of the varpd_plugin_lookup_f, no value is returned. Instead, this
 * is allowed to be an asynchronous operation and therefore any thread may call
 * back the status by using the function varpd_plugin_reply. Again, specifying
 * the appropriate VARPD_LOOKUP_* flags.
 *
 * The flag, VARPD_LOOKUP_OK indicates that the overlay_target_point_t has been
 * filled in completely. The flag, VARPD_LOOKUP_DROP indicates that the packet
 * in question should be dropped.
 */
#define	VARPD_LOOKUP_OK		(0)
#define	VARPD_LOOKUP_DROP	(-1)
typedef int (*varpd_plugin_default_f)(void *, overlay_target_point_t *);
typedef void (*varpd_plugin_lookup_f)(void *, varpd_query_handle_t *,
    const overlay_targ_lookup_t *, overlay_target_point_t *);

/*
 * Do a proxy ARP/NDP lookup.
 */
#define	VARPD_QTYPE_ETHERNET	0x0
typedef void (*varpd_plugin_arp_f)(void *, varpd_arp_handle_t *, int,
    const struct sockaddr *, uint8_t *);

typedef void (*varpd_plugin_dhcp_f)(void *, varpd_dhcp_handle_t *, int,
    const overlay_targ_lookup_t *, uint8_t *);

/*
 * The following four functions all revolve around properties that exist for
 * varpd. A plugin should strive to have a uniform set of properties that exist,
 * however a given plugin may not always support every property. For example, in
 * a vxlan world, the target IP address and port are both required; however,
 * there are other encapsulation protocols which only require an IP address, or
 * maybe require something else.
 */

/*
 * Obtain a total number of properties.
 */
typedef int (*varpd_plugin_nprops_f)(void *, uint_t *);

/*
 * Obtain information about a property.
 */
typedef int (*varpd_plugin_propinfo_f)(void *, const uint_t,
    varpd_prop_handle_t *);

/*
 * Get the value for a single property.
 */
typedef int (*varpd_plugin_getprop_f)(void *, const char *, void *, uint32_t *);

/*
 * Set the value for a single property.
 */
typedef int (*varpd_plugin_setprop_f)(void *, const char *, const void *,
    const uint32_t);

/*
 * Save a plugin's private data into an nvlist.
 */
typedef int (*varpd_plugin_save_f)(void *, nvlist_t *);

/*
 * Restore a plugin's private data to an nvlist.
 */
typedef int (*varpd_plugin_restore_f)(nvlist_t *, varpd_provider_handle_t *,
    overlay_plugin_dest_t, void **);

typedef struct varpd_plugin_ops {
	uint_t			vpo_callbacks;
	varpd_plugin_create_f	vpo_create;
	varpd_plugin_start_f	vpo_start;
	varpd_plugin_stop_f	vpo_stop;
	varpd_plugin_destroy_f	vpo_destroy;
	varpd_plugin_default_f	vpo_default;
	varpd_plugin_lookup_f	vpo_lookup;
	varpd_plugin_nprops_f	vpo_nprops;
	varpd_plugin_propinfo_f	vpo_propinfo;
	varpd_plugin_getprop_f	vpo_getprop;
	varpd_plugin_setprop_f	vpo_setprop;
	varpd_plugin_save_f	vpo_save;
	varpd_plugin_restore_f	vpo_restore;
	varpd_plugin_arp_f	vpo_arp;
	varpd_plugin_dhcp_f	vpo_dhcp;
} varpd_plugin_ops_t;

typedef struct varpd_plugin_register {
	uint_t		vpr_version;
	uint_t		vpr_mode;
	const char	*vpr_name;
	const varpd_plugin_ops_t *vpr_ops;
} varpd_plugin_register_t;

extern varpd_plugin_register_t *libvarpd_plugin_alloc(uint_t, int *);
extern void libvarpd_plugin_free(varpd_plugin_register_t *);
extern int libvarpd_plugin_register(varpd_plugin_register_t *);

/*
 * Blowing up and logging
 */
extern void libvarpd_panic(const char *, ...) __NORETURN;
extern const bunyan_logger_t *libvarpd_plugin_bunyan(varpd_provider_handle_t *);

/*
 * Misc. Information APIs
 */
extern uint64_t libvarpd_plugin_vnetid(varpd_provider_handle_t *);

/*
 * Lookup Replying query and proxying
 */
extern void libvarpd_plugin_query_reply(varpd_query_handle_t *, int);

extern void libvarpd_plugin_proxy_arp(varpd_provider_handle_t *,
    varpd_query_handle_t *, const overlay_targ_lookup_t *);
extern void libvarpd_plugin_proxy_ndp(varpd_provider_handle_t *,
    varpd_query_handle_t *, const overlay_targ_lookup_t *);
extern void libvarpd_plugin_arp_reply(varpd_arp_handle_t *, int);

extern void libvarpd_plugin_proxy_dhcp(varpd_provider_handle_t *,
    varpd_query_handle_t *, const overlay_targ_lookup_t *);
extern void libvarpd_plugin_dhcp_reply(varpd_dhcp_handle_t *, int);


/*
 * Property information callbacks
 */
extern void libvarpd_prop_set_name(varpd_prop_handle_t *, const char *);
extern void libvarpd_prop_set_prot(varpd_prop_handle_t *, overlay_prop_prot_t);
extern void libvarpd_prop_set_type(varpd_prop_handle_t *, overlay_prop_type_t);
extern int libvarpd_prop_set_default(varpd_prop_handle_t *, void *, ssize_t);
extern void libvarpd_prop_set_nodefault(varpd_prop_handle_t *);
extern void libvarpd_prop_set_range_uint32(varpd_prop_handle_t *, uint32_t,
    uint32_t);
extern void libvarpd_prop_set_range_str(varpd_prop_handle_t *, const char *);

/*
 * Various injecting and invalidation routines
 */
extern void libvarpd_inject_varp(varpd_provider_handle_t *, const uint8_t *,
    const overlay_target_point_t *);
extern void libvarpd_inject_arp(varpd_provider_handle_t *, const uint16_t,
    const uint8_t *, const struct in_addr *, const uint8_t *);
extern void libvarpd_fma_degrade(varpd_provider_handle_t *, const char *);
extern void libvarpd_fma_restore(varpd_provider_handle_t *);
/* TODO NDP */

#ifdef __cplusplus
}
#endif

#endif /* _LIBVARPD_PROVIDER_H */
