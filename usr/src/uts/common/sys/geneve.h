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

#ifndef _SYS_GENEVE_H
#define	_SYS_GENEVE_H

/*
 * Common GENEVE information
 */

#include <sys/inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef	_KERNEL

/* Sizes in bytes */
#define	GENEVE_HDR_MIN	8
#define	GENEVE_HDR_MAX	20
#define	GENEVE_ID_LEN	3

#define	GENEVE_OPT_MASK		0x3f00
#define	GENEVE_OPT_SHIFT	8

#define	GENEVE_VERSION		0
#define	GENEVE_VERS_MASK	0xc000
#define	GENEVE_VERS_SHIFT	14

#define	GENEVE_F_OAM		0x0080
#define	GENEVE_F_COPT		0x0040

#define	GENEVE_ID_SHIFT	8
#define	GENEVE_PROT_ETHERNET	0x6558

#pragma pack(1)
typedef struct geneve_hdr {
	uint16_t geneve_flags;
	uint16_t geneve_prot;
	uint32_t geneve_id;
	uint8_t geneve_opts[];
} geneve_hdr_t;
#pragma pack()

#endif	/* _KERNEL */

#ifdef __cplusplus
}
#endif

#endif /* _SYS_GENEVE_H */
