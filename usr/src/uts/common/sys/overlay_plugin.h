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

#ifndef _SYS_OVERLAY_PLUGIN_H
#define	_SYS_OVERLAY_PLUGIN_H

/*
 * Plugin interface for encapsulation/decapsulation modules
 * XXX This is probably totally wrong
 */

#include <sys/stream.h>
#include <sys/mac_provider.h>
#include <sys/overlay_common.h>

#ifdef __cplusplus
extern "C" {
#endif

#define	OVEP_VERSION	0x1

typedef enum overlay_plugin_flags {
	OVEP_F_VLAN_TAG	= 0x01,	/* Supports VLAN Tags */
	/*
	 * XXX STT has a weird property where it can have a VLAN ID, but it is
	 * not allowed to be a part of the encapsulated packet. Though compared
	 * to its abuse of tcp of sorts, it shouldn't really surprise us. We
	 * probably won't care about this for real, but it's here for now.
	 */
	OVEP_F_STRIP_TAG = 0x02	/* VLAN tag should be stripped for encap */
} overlay_plugin_flags_t;

typedef struct ovep_encap_info {
	/*
	 * XXX The ID space could easily be more than a 64-bit number, even
	 * though today it's either a 24-64 bit value. How should we future
	 * proof ourselves here?
	 */
	uint64_t	ovdi_id;
	size_t		ovdi_hdr_size;
	/* XXX Geneve supports non-Ethernet, not sure why we would */
	int		ovdi_encap_type;
	/* XXX STT Doesn't have a vlan present in it by default */
	int		ovdi_vlan;
	/*
	 * XXX NVGRE requires an 8-bit hash, UDP ports generally want a 16-bit
	 * hash which is used for entropy. I guess we should pass in a 16-64 bit
	 * hash and truncate it for NVGRE? We can also set it to zero, but given
	 * its use by ECMP or theoretical use at least, we probably shouldn't.
	 * XXX Is a hash space going to be uniform when truncated in half?
	 */
	uint16_t	ovdi_hash;
} ovep_encap_info_t;

/* XXX These are total strawmen */
/*
 * XXX Some of these protocols, aka geneve, have defined themselves to have
 * options available to them. Of course, none of them are currently defined, but
 * that likely means that we're going to want to have a way to instantiate and
 * set properties on these things, so we'll probably want to have the first
 * argument turn into a void * and add a create and destroy endpoint that gets
 * given the corresponding mac handle.
 */
typedef struct __overlay_prop_handle *overlay_prop_handle_t;
typedef struct __overlay_handle *overlay_handle_t;

/*
 * Plugins are guaranteed that calls to setprop are serialized. However, any
 * number of calls can be going on in parallel otherwise.
 */
typedef int (*overlay_plugin_encap_t)(void *, mblk_t *,
    ovep_encap_info_t *, mblk_t **);
typedef int (*overlay_plugin_decap_t)(void *, mblk_t *,
    ovep_encap_info_t *);
typedef int (*overlay_plugin_init_t)(overlay_handle_t, void **);
typedef void (*overlay_plugin_fini_t)(void *);
typedef int (*overlay_plugin_socket_t)(void *, int *, int *, int *,
    struct sockaddr *, socklen_t *);
typedef int (*overlay_plugin_getprop_t)(void *, const char *, void *,
    uint32_t *);
typedef int (*overlay_plugin_setprop_t)(void *, const char *, const void *,
    uint32_t);
typedef int (*overlay_plugin_propinfo_t)(const char *, overlay_prop_handle_t);

typedef struct overlay_plugin_ops {
	uint_t			ovpo_callbacks;
	overlay_plugin_init_t	ovpo_init;
	overlay_plugin_fini_t	ovpo_fini;
	overlay_plugin_encap_t	ovpo_encap;
	overlay_plugin_decap_t	ovpo_decap;
	overlay_plugin_socket_t ovpo_socket;
	overlay_plugin_getprop_t ovpo_getprop;
	overlay_plugin_setprop_t ovpo_setprop;
	overlay_plugin_propinfo_t ovpo_propinfo;
} overlay_plugin_ops_t;

typedef struct overlay_plugin_register {
	uint_t			ovep_version;
	const char		*ovep_name;
	const overlay_plugin_ops_t	*ovep_ops;
	const char 		**ovep_props;
	uint_t			ovep_id_size;
	uint_t			ovep_flags;
	uint_t			ovep_hdr_min;
	uint_t			ovep_hdr_max;
	uint_t			ovep_dest;
} overlay_plugin_register_t;

/*
 * Functions that interact with registration
 */
extern overlay_plugin_register_t *overlay_plugin_alloc(uint_t);
extern void overlay_plugin_free(overlay_plugin_register_t *);
extern int overlay_plugin_register(overlay_plugin_register_t *);
extern int overlay_plugin_unregister(const char *);

/*
 * Property information callbacks
 */
extern void overlay_prop_set_name(overlay_prop_handle_t, const char *);
extern void overlay_prop_set_prot(overlay_prop_handle_t, overlay_prop_prot_t);
extern void overlay_prop_set_type(overlay_prop_handle_t, overlay_prop_type_t);
extern int overlay_prop_set_default(overlay_prop_handle_t, void *, ssize_t);
extern void overlay_prop_set_nodefault(overlay_prop_handle_t);
extern void overlay_prop_set_range_uint32(overlay_prop_handle_t, uint32_t,
    uint32_t);
extern void overlay_prop_set_range_str(overlay_prop_handle_t, const char *);

/*
 * Callbacks that should be made -- without locks held by the user.
 */


#ifdef __cplusplus
}
#endif

#endif /* _SYS_OVERLAY_PLUGIN_H */
