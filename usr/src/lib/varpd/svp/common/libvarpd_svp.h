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

#ifndef _LIBVARPD_SVP_H
#define	_LIBVARPD_SVP_H

/*
 * Implementation details of the SVP plugin and the SVP protocol.
 */

#include <netinet/in.h>
#include <sys/ethernet.h>
#include <thread.h>
#include <synch.h>
#include <libvarpd_provider.h>
#include <sys/avl.h>
#include <port.h>
#include <sys/list.h>
#include <bunyan.h>

#include <libvarpd_svp_prot.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct svp svp_t;
typedef struct svp_remote svp_remote_t;
typedef struct svp_conn svp_conn_t;
typedef struct svp_query svp_query_t;

typedef void (*svp_event_f)(port_event_t *, void *);

typedef struct svp_event {
	svp_event_f	se_func;
	void		*se_arg;
	int		se_events;
} svp_event_t;

typedef void (*svp_timer_f)(void *);

typedef struct svp_timer {
	svp_timer_f	st_func;
	void		*st_arg;
	boolean_t	st_oneshot;
	uint32_t	st_value;
	/* Fields below here are private to the svp_timer implementaiton */
	uint64_t	st_expire;
	boolean_t	st_delivering;
	avl_node_t	st_link;
} svp_timer_t;

typedef union svp_query_data {
	svp_vl2_req_t	sqd_vl2r;
	svp_vl2_ack_t	sqd_vl2a;
	svp_vl3_req_t	sdq_vl3r;
	svp_vl3_ack_t	sdq_vl3a;
} svp_query_data_t;

typedef void (*svp_query_f)(svp_query_t *, void *);

typedef enum svp_query_state {
	SVP_QUERY_INIT		= 0x00,
	SVP_QUERY_WRITING	= 0x01,
	SVP_QUERY_READING	= 0x02,
	SVP_QUERY_FINISHED	= 0x03
} svp_query_state_t;

/*
 * The query structure is usable for all forms of svp queries that end up
 * getting passed across. Right now it's optimized for the fixed size data
 * requests as opposed to requests whose responses will always be streaming in
 * nature. Though, the streaming requests are the less common ones we have.
 *
 * XXX Fix that and make this streaming friendly
 */
struct svp_query {
	list_node_t		sq_lnode;
	svp_query_f		sq_func;
	svp_query_state_t	sq_state;
	void			*sq_arg;
	svp_t			*sq_svp;
	svp_req_t		sq_header;
	svp_query_data_t	sq_rdun;
	svp_query_data_t	sq_wdun;
	svp_status_t		sq_status;
	void			*sq_rdata;
	size_t			sq_rsize;
	void			*sq_wdata;
	size_t			sq_wsize;
	hrtime_t		sq_acttime;
};

/*
 * XXX Centralize this somewhere more generally, big theroy statement, where are
 * you?
 *
 * We have a connection pool that's built upon DNS records. DNS describes the
 * membership of the set of remote peers that make up our pool and we maintain
 * one connection to each of them.  In addition, we maintain an exponential
 * backoff for each peer and will attempt to reconect immediately before backing
 * off. The following are the valid states that a connection can be in:
 *
 *	SVP_CS_INITIAL		This is the initial state of a connection, all
 *				that should exist is an unbound socket.
 *
 *	SVP_CS_CONNECTING	A call to connect has been made and we are
 *				polling for it to complete.
 *
 *	SVP_CS_BACKOFF		A connect attempt has failed and we are
 *				currently backing off, waiting to try again.
 *
 *	SVP_CS_ACTIVE		We have successfully connected to the remote
 *				system.
 *
 *	SVP_CS_WINDDOWN		This connection is going to valhalla. In other
 *				words, a previously active connection is no
 *				longer valid in DNS, so we should curb our use
 *				of it, and reap it as soon as we have other
 *				active connections.
 *
 *	SVP_CS_REAPING		This connection object will be freed and reaped.
 *				It will no longer be used.
 *
 * The following diagram attempts to describe our state transition scheme, and
 * when we transition from one state to the next.
 *
 *                               |
 *                               * New remote IP from DNS resolution,
 *                               | not currently active in the system.
 *                               |
 *                               v                                Socket Error,
 *                       +----------------+                       still in DNS
 *                       | SVP_CS_INITIAL |<----------------------*-----+
 *                       +----------------+                             |
 *                                       |                              |
 *    Connection failed ..        Always *               Successful     |
 *    backoff limit      .               |               connect()      |
 *    not exceeded  +----*---------+     |        +-----------*--+      |
 *                  |              |     |        |              |      |
 *                  V              ^     v        ^              V      ^
 *     +----------------+         +-------------------+     +---------------+
 * +-<-| SVP_CS_BACKOFF |         | SVP_CS_CONNECTING |     | SVP_CS_ACTIVE |
 * |   +----------------+         +-------------------+     +---------------+
 * |                V              ^  |                       ^    V
 * |  Backoff wait  *              |  v                       |    * Removed
 * |  interval      +--------------+  |              Added to *    | from DNS
 * |  finished                        |              DNS      |    |
 * |                                  |                       |    |
 * |                                  |                       ^    V
 * |                                  |            +-----------------+
 * +---->---------------+-----<-------+        +-<-| SVP_CS_WINDDOWN |
 *                      v                 Conn *   +-----------------+
 *                      |                Error |                V
 *         Removed from *                      v                |
 *         DNS          |            +----------------+         * Connection
 *                      +----------->| SVP_CS_REAPING |<--------+ Quiesced
 *                                   +----------------+
 *
 */
typedef enum svp_conn_state {
	SVP_CS_ERROR		= 0x00,
	SVP_CS_INITIAL		= 0x01,
	SVP_CS_CONNECTING	= 0x02,
	SVP_CS_BACKOFF		= 0x03,
	SVP_CS_ACTIVE		= 0x04,
	SVP_CS_WINDDOWN		= 0x05
} svp_conn_state_t;

typedef enum svp_conn_error {
	SVP_CE_NONE		= 0x00,
	SVP_CE_ASSOCIATE	= 0x01,
	SVP_CE_NOPOLLOUT	= 0x02,
	SVP_CE_SOCKET		= 0x03
} svp_conn_error_t;

typedef enum svp_conn_flags {
	SVP_CF_ADDED		= 0x01,
	SVP_CF_DEGRADED		= 0x02,
	SVP_CF_REAP		= 0x04,
	SVP_CF_TEARDOWN		= 0x08,
	SVP_CF_UFLAG		= 0x0c,
	SVP_CF_USER		= 0x10
} svp_conn_flags_t;

typedef struct svp_conn_out {
	svp_query_t		*sco_query;
	size_t			sco_offset;
} svp_conn_out_t;

typedef struct svp_conn_in {
	svp_query_t 		*sci_query;
	svp_req_t		sci_req;
	size_t			sci_offset;
} svp_conn_in_t;

struct svp_conn {
	svp_remote_t		*sc_remote;	/* RO */
	struct in6_addr		sc_addr;	/* RO */
	list_node_t		sc_rlist;	/* svp_remote_t`sr_lock */
	mutex_t			sc_lock;
	svp_event_t		sc_event;
	svp_timer_t		sc_btimer;
	svp_timer_t		sc_qtimer;
	int			sc_socket;
	uint_t			sc_gen;
	uint_t			sc_nbackoff;
	svp_conn_flags_t	sc_flags;
	svp_conn_state_t	sc_cstate;
	svp_conn_error_t	sc_error;
	int			sc_errno;
	hrtime_t		sc_lastact;
	list_t			sc_queries;
	svp_conn_out_t		sc_output;
	svp_conn_in_t		sc_input;
};

typedef enum svp_remote_state {
	SVP_RS_LOOKUP_SCHEDULED		= 0x01,	/* On the DNS Queue */
	SVP_RS_LOOKUP_INPROGRESS 	= 0x02,	/* Doing a DNS lookup */
	SVP_RS_LOOKUP_VALID		= 0x04	/* addrinfo valid */
} svp_remote_state_t;

/*
 * These series of bit-based flags should be ordered such that the most severe
 * is first. We only can set one message that user land can see, so if more than
 * one is set we want to make sure that one is there.
 */
typedef enum svp_degrade_state {
	SVP_RD_DNS_FAIL		= 0x01,	/* DNS Resolution Failure */
	SVP_RD_REMOTE_FAIL	= 0x02,	/* cannot reach any remote peers */
	SVP_RD_ALL		= 0x03	/* Only suitable for restore */
} svp_degrade_state_t;

struct svp_remote {
	char			*sr_hostname;	/* RO */
	uint16_t		sr_rport;	/* RO */
	avl_node_t		sr_gnode;	/* svp_remote_lock */
	svp_remote_t		*sr_nexthost;	/* svp_host_lock */
	mutex_t			sr_lock;
	svp_remote_state_t	sr_state;
	svp_degrade_state_t	sr_degrade;
	struct addrinfo 	*sr_addrinfo;
	avl_tree_t		sr_tree;
	uint_t			sr_count;	/* active count */
	uint_t			sr_gen;
	uint_t			sr_tconns;	/* total conns + dconns */
	uint_t			sr_ndconns;	/* number of degraded conns */
	list_t			sr_conns;	/* all conns */
};

/*
 * We have a bunch of different things that we get back from the API at the
 * plug-in layer. These include:
 *
 *   o OOB Shootdowns
 *   o VL3->VL2 Lookups
 *   o VL2->UL3 Lookups
 *   o VL2 Log invalidations
 *   o VL3 Log injections
 */
typedef void (*svp_vl2_lookup_f)(svp_t *, svp_status_t, const struct in6_addr *,
    const uint16_t, void *);
typedef void (*svp_vl3_lookup_f)(svp_t *, svp_status_t, const uint8_t *,
    const struct in6_addr *, const uint16_t, void *);
typedef void (*svp_vl2_invalidation_f)(svp_t *, const uint8_t *);
typedef void (*svp_vl3_inject_f)(svp_t *, const uint16_t,
    const struct in6_addr *, const uint8_t *, const uint8_t *);
typedef void (*svp_shootdown_f)(svp_t *, const uint8_t *,
    const struct in6_addr *, const uint16_t uport);

typedef struct svp_cb {
	svp_vl2_lookup_f	scb_vl2_lookup;
	svp_vl3_lookup_f	scb_vl3_lookup;
	svp_vl2_invalidation_f	scb_vl2_invalidate;
	svp_vl3_inject_f	scb_vl3_inject;
	svp_shootdown_f		scb_shootdown;
} svp_cb_t;

/*
 * Core implementation structure.
 */
struct svp {
	overlay_plugin_dest_t	svp_dest;	/* RO */
	varpd_provider_handle_t	*svp_hdl;	/* RO */
	svp_cb_t		svp_cb;		/* RO */
	uint64_t		svp_vid;	/* RO? */
	avl_node_t 		svp_rlink;	/* Owned by svp_remote */
	svp_remote_t		*svp_remote;	/* ROish XXX */
	mutex_t			svp_lock;
	char			*svp_host;
	uint16_t		svp_port;
	uint16_t		svp_uport;
	boolean_t		svp_huip;
	struct in6_addr		svp_uip;
};

extern bunyan_logger_t *svp_bunyan;

/*
 * XXX Strawman backend APIs
 */
extern int svp_remote_find(char *, uint16_t, svp_remote_t **);
extern int svp_remote_attach(svp_remote_t *, svp_t *);
extern void svp_remote_detach(svp_t *);
extern void svp_remote_release(svp_remote_t *);
extern void svp_remote_vl3_lookup(svp_t *, svp_query_t *,
    const struct sockaddr *, void *);
extern void svp_remote_vl2_lookup(svp_t *, svp_query_t *, const uint8_t *,
    void *);

/*
 * Init functions
 */
extern int svp_remote_init(void);
extern void svp_remote_fini(void);
extern int svp_event_init(void);
extern int svp_event_timer_init(svp_event_t *);
extern void svp_event_fini(void);
extern int svp_host_init(void);
extern int svp_timer_init(void);

/*
 * Timers
 */
extern int svp_tickrate;
extern void svp_timer_add(svp_timer_t *);
extern void svp_timer_remove(svp_timer_t *);

/*
 * Event loop management
 */
extern int svp_event_associate(svp_event_t *, int);
extern int svp_event_dissociate(svp_event_t *, int);
extern int svp_event_inject(void *);

/*
 * Connection manager
 */
extern int svp_conn_create(svp_remote_t *, const struct in6_addr *);
extern void svp_conn_destroy(svp_conn_t *);
extern void svp_conn_fallout(svp_conn_t *);
extern void svp_conn_queue(svp_conn_t *, svp_query_t *);

/*
 * FMA related
 */
extern void svp_remote_degrade(svp_remote_t *, svp_degrade_state_t);
extern void svp_remote_restore(svp_remote_t *, svp_degrade_state_t);

/*
 * Misc.
 */
extern int svp_comparator(const void *, const void *);
extern void svp_remote_reassign(svp_remote_t *, svp_conn_t *);
extern void svp_remote_resolved(svp_remote_t *, struct addrinfo *);
extern void svp_host_queue(svp_remote_t *);
extern void svp_query_release(svp_query_t *);
extern void svp_query_crc32(svp_req_t *, void *, size_t);

#ifdef __cplusplus
}
#endif

#endif /* _LIBVARPD_SVP_H */
