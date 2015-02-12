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

#ifndef _SYS_NVGRE_H
#define	_SYS_NVGRE_H

/*
 * Common NVGRE information
 */

#include <sys/inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef	_KERNEL

/* Sizes in bytes */
#define	NVGRE_HDR_LEN	8
#define	NVGRE_ID_LEN	3

#define	NVGRE_FLAG_MASK	0xb007
#define	NVGRE_FLAG_VALUE	0x2000
#define	NVGRE_PROTOCOL	0x6558
#define	NVGRE_ID_MASK	0xffffff00
#define	NVGRE_ID_SHIFT	8
#define	NVGRE_FLOW_MASK	0x000000ff

#pragma pack(1)
typedef struct nvgre_hdr {
	uint16_t nvgre_flags;
	uint16_t nvgre_prot;
	uint32_t nvgre_id;
} nvgre_hdr_t;
#pragma pack()

#endif	/* _KERNEL */

#ifdef __cplusplus
}
#endif

#endif /* _SYS_NVGRE_H */
