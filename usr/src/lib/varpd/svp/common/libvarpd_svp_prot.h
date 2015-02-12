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

#ifndef _LIBVARPD_SVP_PROT_H
#define	_LIBVARPD_SVP_PROT_H

/*
 * SVP protocol Definitions
 */

#include <sys/types.h>
#include <inttypes.h>
#include <sys/ethernet.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SDC VXLAN Protocol Definitions
 */

#define	SVP_VERSION_ONE	1
#define	SVP_CURRENT_VERSION	SVP_VERSION_ONE

typedef struct svp_req {
	uint16_t	svp_ver;
	uint16_t	svp_op;
	uint32_t	svp_size;
	uint32_t	svp_id;
	uint32_t	svp_crc32;
} svp_req_t;

typedef enum svp_op {
	SVP_R_UNKNOWN	= 0x00,
	SVP_R_PING	= 0x01,
	SVP_R_PONG	= 0x02,
	SVP_R_VL2_REQ	= 0x03,
	SVP_R_VL2_ACK	= 0x04,
	SVP_R_VL3_REQ	= 0x05,
	SVP_R_VL3_ACK	= 0x06,
	SVP_R_BULK_REQ	= 0x07,
	SVP_R_BULK_ACK	= 0x08,
	SVP_R_LOG_REQ	= 0x09,
	SVP_R_LOG_ACK	= 0x0A,
	SVP_R_LOG_RM	= 0x0B,
	SVP_R_LOG_RACK	= 0x0C,
	SVP_R_SHOOTDOWN	= 0x0D
} svp_op_t;

typedef enum svp_status {
	SVP_S_OK	= 0x00,	/* Everything OK */
	SVP_S_FATAL	= 0x01,	/* Fatal error, close connection */
	SVP_S_NOTFOUND	= 0x02,	/* Entry not found */
	SVP_S_BADL3TYPE	= 0x03,	/* Unknown svp_vl3_type_t */
	SVP_S_BADBULK	= 0x04,	/* Unknown svp_bulk_type_t */
	SVP_S_BADLOG	= 0x05,	/* Unknown svp_log_type_t */
	SVP_S_LOGAGAIN	= 0x06	/* Nothing in the log yet */
} svp_status_t;

typedef struct svp_vl2_req {
	uint8_t		sl2r_mac[ETHERADDRL];
	uint8_t		sl2r_pad[2];
	uint32_t	sl2r_vnetid;
} svp_vl2_req_t;

typedef struct svp_vl2_ack {
	uint16_t	sl2a_status;
	uint16_t	sl2a_port;
	uint8_t		sl2a_addr[16];
} svp_vl2_ack_t;

typedef enum svp_vl3_type {
	SVP_VL3_IP	= 0x01,
	SVP_VL3_IPV6	= 0x02
} svp_vl3_type_t;

typedef struct svp_vl3_req {
	uint8_t		sl3r_ip[16];
	uint32_t	sl3r_type;
	uint32_t	sl3r_vnetid;
} svp_vl3_req_t;

typedef struct svp_vl3_ack {
	uint32_t	sl3a_status;
	uint8_t		sl3a_mac[ETHERADDRL];
	uint16_t	sl3a_uport;
	uint8_t		sl3a_uip[16];
} svp_vl3_ack_t;

typedef enum svp_bulk_type {
	SVP_BULK_VL2	= 0x01,
	SVP_BULK_VL3	= 0x02
} svp_bulk_type_t;

typedef struct svp_bulk_req {
	uint32_t	svbr_type;
} svp_bulk_req_t;

typedef struct svp_bulk_ack {
	uint32_t	svba_status;
	uint32_t	svba_type;
	uint8_t		svba_data[];
} svp_bulk_ack_t;

typedef enum svp_log_type {
	SVP_LOG_VL2	= 0x01,
	SVP_LOG_VL3	= 0x02
} svp_log_type_t;

typedef struct svp_log_req {
	uint32_t	svlr_type;
	uint32_t	svlr_count;
} svp_log_req_t;

typedef struct svp_log_vl2 {
	uint8_t		svl2_id[16];	/* 16-byte UUID */
	uint8_t		svl2_mac[ETHERADDRL];
	uint8_t		svl2_pad[2];
	uint32_t	svl2_vnetid;
} svp_log_vl2_t;

typedef struct svp_log_vl3 {
	uint8_t		svl3_id[16];	/* 16-byte UUID */
	uint8_t		slv3_ip[16];
	uint8_t		svl3_mac[ETHERADDRL];
	uint16_t	svl3_vlan;
	uint8_t		svl3_tmac[ETHERADDRL];
	uint8_t		svl3_tpad[2];
	uint32_t	svl3_vnetid;
} svp_log_vl3_t;

typedef struct svp_log_ack {
	uint32_t	svla_status;
	uint32_t	svla_type;
	uint8_t		svla_data[];
} svp_log_ack_t;

typedef struct svp_lrm_req {
	uint32_t	svrr_type;
	uint32_t	svrr_pad;
	uint8_t		svrr_ids[];
} svp_lrm_req_t;

typedef struct svp_lrm_ack {
	uint32_t	svra_status;
} svp_lrm_ack_t;

typedef struct svp_shootdown {
	uint8_t		svsd_mac[ETHERADDRL];
	uint8_t		svsd_pad[2];
	uint32_t	svsd_vnetid;
} svp_shootdown_t;

#ifdef __cplusplus
}
#endif

#endif /* _LIBVARPD_SVP_PROT_H */
