/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2014 Nexenta Systems, Inc.  All rights reserved.
 */

#include <syslog.h>
#include <synch.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <sys/errno.h>

#include <smbsrv/libsmb.h>
#include <smbsrv/libsmbns.h>
#include <smbsrv/libmlsvc.h>
#include <smbsrv/smbinfo.h>
#include "smbd.h"

#define	SMBD_DC_MONITOR_ATTEMPTS		3
#define	SMBD_DC_MONITOR_RETRY_INTERVAL		3	/* seconds */
#define	SMBD_DC_MONITOR_INTERVAL		60	/* seconds */

extern smbd_t smbd;

static mutex_t smbd_dc_mutex;
static cond_t smbd_dc_cv;

static void *smbd_dc_monitor(void *);
static void smbd_dc_update(void);
/* Todo: static boolean_t smbd_set_netlogon_cred(void); */
static void smbd_join_workgroup(smb_joininfo_t *, smb_joinres_t *);
static void smbd_join_domain(smb_joininfo_t *, smb_joinres_t *);

/*
 * Launch the DC discovery and monitor thread.
 */
int
smbd_dc_monitor_init(void)
{
	pthread_attr_t	attr;
	int		rc;

	(void) smb_config_getstr(SMB_CI_ADS_SITE, smbd.s_site,
	    MAXHOSTNAMELEN);
	(void) smb_config_getip(SMB_CI_DOMAIN_SRV, &smbd.s_pdc);
	smb_ads_init();

	if (smbd.s_secmode != SMB_SECMODE_DOMAIN)
		return (0);

	(void) pthread_attr_init(&attr);
	(void) pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	rc = pthread_create(&smbd.s_dc_monitor_tid, &attr, smbd_dc_monitor,
	    NULL);
	(void) pthread_attr_destroy(&attr);
	return (rc);
}

/*
 * Refresh the DC monitor.  Called from SMF refresh and when idmap
 * finds a different DC from what we were using previously.
 * Update our domain (and current DC) information.
 */
void
smbd_dc_monitor_refresh(void)
{

	syslog(LOG_INFO, "smbd_dc_monitor_refresh");

	(void) mutex_lock(&smbd_dc_mutex);

	smbd.s_pdc_changed = B_TRUE;
	(void) cond_signal(&smbd_dc_cv);

	(void) mutex_unlock(&smbd_dc_mutex);
}

/*ARGSUSED*/
static void *
smbd_dc_monitor(void *arg)
{
	boolean_t	ds_not_responding;
	boolean_t	ds_cfg_changed;
	timestruc_t	delay;
	int		i;

	smbd_dc_update();
	smbd_online_wait("smbd_dc_monitor");

	while (smbd_online()) {
		ds_not_responding = B_FALSE;
		ds_cfg_changed = B_FALSE;
		delay.tv_sec = SMBD_DC_MONITOR_INTERVAL;
		delay.tv_nsec = 0;

		(void) mutex_lock(&smbd_dc_mutex);
		(void) cond_reltimedwait(&smbd_dc_cv, &smbd_dc_mutex, &delay);

		if (smbd.s_pdc_changed) {
			smbd.s_pdc_changed = B_FALSE;
			ds_cfg_changed = B_TRUE;
		}

		(void) mutex_unlock(&smbd_dc_mutex);

		if (ds_cfg_changed)
			goto rediscover;

		for (i = 0; i < SMBD_DC_MONITOR_ATTEMPTS; ++i) {
			if (dssetup_check_service() == 0) {
				ds_not_responding = B_FALSE;
				break;
			}

			ds_not_responding = B_TRUE;
			(void) sleep(SMBD_DC_MONITOR_RETRY_INTERVAL);
		}

		if (ds_not_responding)
			smb_log(smbd.s_loghd, LOG_NOTICE,
			    "smbd_dc_monitor: domain service not responding");

		if (ds_not_responding || ds_cfg_changed) {
		rediscover:
			smb_ads_refresh(ds_not_responding);
			smbd_dc_update();
		}
	}

	smbd.s_dc_monitor_tid = 0;
	return (NULL);
}

/*
 * Locate a domain controller in the current resource domain and Update
 * the Netlogon credential chain.
 *
 * The domain configuration will be updated upon successful DC discovery.
 */
static void
smbd_dc_update(void)
{
	char		domain[MAXHOSTNAMELEN];
	smb_domainex_t	info;
	smb_domain_t	*di;
	DWORD		status;

	if (smb_getfqdomainname(domain, MAXHOSTNAMELEN) != 0) {
		(void) smb_getdomainname(domain, MAXHOSTNAMELEN);
	}
	if (domain[0] == '\0') {
		smb_log(smbd.s_loghd, LOG_NOTICE,
		    "smbd_dc_update: no domain name set");
		return;
	}

	if (!smb_locate_dc(domain, "", &info)) {
		smb_log(smbd.s_loghd, LOG_NOTICE,
		    "smbd_dc_update: %s: locate failed", domain);
		return;
	}

	di = &info.d_primary;
	smb_log(smbd.s_loghd, LOG_INFO,
	    "smbd_dc_update: %s: located %s", domain, info.d_dc);

	status = mlsvc_netlogon(info.d_dc, di->di_nbname);
	if (status != NT_STATUS_SUCCESS) {
		syslog(LOG_NOTICE,
		    "failed to establish NETLOGON credential chain");

		/*
		 * Restart required because the domain changed
		 * or the credential chain setup failed.
		 */
		smb_log(smbd.s_loghd, LOG_NOTICE,
		    "smbd_dc_update: smb/server restart required");

		if (smb_smf_restart_service() != 0)
			smb_log(smbd.s_loghd, LOG_ERR,
			    "restart failed: run 'svcs -xv smb/server'"
			    " for more information");
	}
}

/*
 * smbd_join
 *
 * Joins the specified domain/workgroup.
 *
 * If the security mode or domain name is being changed,
 * the caller must restart the service.
 */
void
smbd_join(smb_joininfo_t *info, smb_joinres_t *res)
{
	dssetup_clear_domain_info();
	if (info->mode == SMB_SECMODE_WORKGRP)
		smbd_join_workgroup(info, res);
	else
		smbd_join_domain(info, res);
}

static void
smbd_join_workgroup(smb_joininfo_t *info, smb_joinres_t *res)
{
	char nb_domain[SMB_PI_MAX_DOMAIN];

	(void) smb_config_getstr(SMB_CI_DOMAIN_NAME, nb_domain,
	    sizeof (nb_domain));

	smbd_set_secmode(SMB_SECMODE_WORKGRP);
	smb_config_setdomaininfo(info->domain_name, "", "", "", "");
	(void) smb_config_set_idmap_domain("");
	(void) smb_config_refresh_idmap();

	if (strcasecmp(nb_domain, info->domain_name))
		smb_browser_reconfig();

	res->status = NT_STATUS_SUCCESS;
}

static void
smbd_join_domain(smb_joininfo_t *info, smb_joinres_t *res)
{

	/* info->domain_name could either be NetBIOS domain name or FQDN */
	mlsvc_join(info, res);
	if (res->status == 0) {
		smbd_set_secmode(SMB_SECMODE_DOMAIN);
	} else {
		syslog(LOG_ERR, "smbd: failed joining %s (%s)",
		    info->domain_name, xlate_nt_status(res->status));
	}
}
