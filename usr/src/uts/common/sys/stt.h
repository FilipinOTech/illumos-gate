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

#ifndef _SYS_STT_H
#define	_SYS_STT_H

/*
 * Common STT information
 */

#include <sys/inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef	_KERNEL

/* Sizes in bytes */
#define	STT_HDR_LEN	18
#define	STT_ID_LEN	8

#define	STT_SET_FLAG(val, f)	((val) |= 1 << (f))
#define	STT_F_CKSUM_VERIFY	0x80
#define	STT_F_CKSUM_PARTIAL	0x40
#define	STT_F_IPV4		0x20
#define	STT_F_ISTCP		0x10
#define	STT_F_RESERVED		0x0f

#define	STT_L4OFF_MAX		0xff

#define	STT_VLAN_VALID		0x1000

#define	STT_VERSION	0

#pragma pack(1)
typedef struct stt_hdr {
	uint8_t stt_version;
	uint8_t stt_flags;
	uint8_t stt_l4off;
	uint8_t stt_reserved;
	uint16_t stt_mss;
	uint16_t stt_vlan;
	uint64_t stt_id;
	uint16_t stt_padding;
} stt_hdr_t;
#pragma pack()

#endif	/* _KERNEL */

#ifdef __cplusplus
}
#endif

#endif /* _SYS_STT_H */
