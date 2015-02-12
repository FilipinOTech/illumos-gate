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

/*
 * DNS Host-name related functions.
 *
 * Every backend is stored in DNS. To find out memebership, we query DNS and use
 * that to update our world. We update our DNS records on both a timer
 * granularity and immediately after creation. We'll also XXX go through and do
 * this after all of our valid entries have disappeared.
 *
 * Unfortuantely, doing host name resolution in a way that allows us to leverage
 * the system resolvers and the system's caching, require us to use blocking
 * calls in libc. If we can't reach a given server, that will tie up a thread
 * for quite some time. To work around that fact, we're going to create a fixed
 * number of threads and we'll use them to service this kind of work. While not
 * great, we don't have many better options.
 */

#include <sys/socket.h>
#include <netdb.h>
#include <thread.h>
#include <synch.h>
#include <assert.h>
#include <errno.h>

#include <libvarpd_svp.h>

int svp_host_nthreads = 8;

static mutex_t svp_host_lock = DEFAULTMUTEX;
static cond_t svp_host_cv = DEFAULTCV;
static svp_remote_t *svp_host_head;

static void *
svp_host_loop(void *unused)
{
	for (;;) {
		int err;
		svp_remote_t *srp;
		struct addrinfo *addrs;

		mutex_lock(&svp_host_lock);
		while (svp_host_head == NULL)
			cond_wait(&svp_host_cv, &svp_host_lock);
		srp = svp_host_head;
		svp_host_head = srp->sr_nexthost;
		if (svp_host_head != NULL)
			cond_signal(&svp_host_cv);
		mutex_unlock(&svp_host_lock);

		mutex_lock(&srp->sr_lock);
		assert(srp->sr_state & SVP_RS_LOOKUP_SCHEDULED);
		srp->sr_state &= ~SVP_RS_LOOKUP_SCHEDULED;
		if (srp->sr_state & SVP_RS_LOOKUP_INPROGRESS) {
			mutex_unlock(&srp->sr_lock);
			continue;
		}
		srp->sr_state |= SVP_RS_LOOKUP_INPROGRESS;
		mutex_unlock(&srp->sr_lock);

		for (;;) {
			err = getaddrinfo(srp->sr_hostname, NULL, NULL, &addrs);
			if (err == 0)
				break;
			if (err != 0) {
				switch (err) {
				case EAI_ADDRFAMILY:
				case EAI_BADFLAGS:
				case EAI_FAMILY:
				case EAI_SERVICE:
				case EAI_SOCKTYPE:
				case EAI_OVERFLOW:
				default:
					libvarpd_panic("unexpected getaddrinfo "
					    "failure: %d", err);
				case EAI_AGAIN:
				case EAI_MEMORY:
				case EAI_SYSTEM:
					continue;
				case EAI_FAIL:
				case EAI_NODATA:
				case EAI_NONAME:
					/*
					 * XXX At this point in time we have
					 * something which isn't very good. This
					 * may have been a typo or something may
					 * have been destroyed. We should go
					 * ahead and degrade this overall
					 * instance, because we're not going to
					 * make much forward progress... It'd be
					 * great if we could actually issue more
					 * of an EREPORT to describe what
					 * happened...
					 */
					mutex_lock(&srp->sr_lock);
					svp_remote_degrade(srp, SVP_RD_DNS_FAIL);
					mutex_unlock(&srp->sr_lock);
					break;
				}
			}
			break;
		}

		if (err == 0) {
			/*
			 * We've successfully resolved something, mark this
			 * degredation over for now.
			 */
			mutex_lock(&srp->sr_lock);
			svp_remote_restore(srp, SVP_RD_DNS_FAIL);
			mutex_unlock(&srp->sr_lock);
			svp_remote_resolved(srp, addrs);
		}

		mutex_lock(&srp->sr_lock);
		srp->sr_state &= ~SVP_RS_LOOKUP_INPROGRESS;
		mutex_unlock(&srp->sr_lock);
	}
}

void
svp_host_queue(svp_remote_t *srp)
{
	svp_remote_t *s;
	mutex_lock(&svp_host_lock);
	mutex_lock(&srp->sr_lock);
	if (srp->sr_state & SVP_RS_LOOKUP_SCHEDULED) {
		mutex_unlock(&srp->sr_lock);
		mutex_unlock(&svp_host_lock);
		return;
	}
	srp->sr_state |= SVP_RS_LOOKUP_SCHEDULED;
	s = svp_host_head;
	while (s != NULL && s->sr_nexthost != NULL)
		s = s->sr_nexthost;
	if (s == NULL) {
		assert(s == svp_host_head);
		svp_host_head = srp;
	} else {
		s->sr_nexthost = srp;
	}
	srp->sr_nexthost = NULL;
	cond_signal(&svp_host_cv);
	mutex_unlock(&srp->sr_lock);
	mutex_unlock(&svp_host_lock);
}

int
svp_host_init(void)
{
	int i;

	for (i = 0; i < svp_host_nthreads; i++) {
		if (thr_create(NULL, 0, svp_host_loop, NULL,
		    THR_DETACHED | THR_DAEMON, NULL) != 0)
			return (errno);
	}

	return (0);
}
