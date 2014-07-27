/*
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 */

/*
 * BSD 3 Clause License
 *
 * Copyright (c) 2007, The Storage Networking Industry Association.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 	- Redistributions of source code must retain the above copyright
 *	  notice, this list of conditions and the following disclaimer.
 *
 * 	- Redistributions in binary form must reproduce the above copyright
 *	  notice, this list of conditions and the following disclaimer in
 *	  the documentation and/or other materials provided with the
 *	  distribution.
 *
 *	- Neither the name of The Storage Networking Industry Association (SNIA)
 *	  nor the names of its contributors may be used to endorse or promote
 *	  products derived from this software without specific prior written
 *	  permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
/* Copyright (c) 2007, The Storage Networking Industry Association. */
/* Copyright (c) 1996, 1997 PDC, Network Appliance. All Rights Reserved */
/* Copyright (c) 2011 by Delphix. All rights reserved. */

/*
 * Handlers for config requests.
 */

#include "ndmp_impl.h"

/*
 * Get information about the current host.   This includes the hostname and
 * various OS-specific information.  This is different from the SERVER INFO
 * request, which includes information about the NDMP server itself.
 */
/*ARGSUSED*/
void
ndmp_config_get_host_info_v3(ndmp_session_t *session, void *body)
{
	ndmp_config_get_host_info_reply_v3 reply = { 0 };
	char buf[HOSTNAMELEN + 1];
	struct utsname uts;
	char hostidstr[16];
	ulong_t hostid;

	buf[0] = '\0';
	(void) gethostname(buf, sizeof (buf));

	reply.hostname = buf;
	(void) uname(&uts);
	reply.os_type = uts.sysname;
	reply.os_vers = uts.release;

	if (sysinfo(SI_HW_SERIAL, hostidstr, sizeof (hostidstr)) < 0) {
		ndmp_log(session, LOG_ERR,
		    "unable to get system serial number");
		reply.error = NDMP_UNDEFINED_ERR;
	}

	/*
	 * Convert the hostid to hex. The returned string must match
	 * the string returned by hostid(1).
	 */
	hostid = strtoul(hostidstr, NULL, 0);
	(void) snprintf(hostidstr, sizeof (hostidstr), "%lx", hostid);
	reply.hostid = hostidstr;

	ndmp_send_reply(session, &reply);
}

/*
 * Get information about a particular authorization type, which really amounts
 * to just returning the MD5 challenge information.
 */
void
ndmp_config_get_auth_attr_v3(ndmp_session_t *session, void *body)
{
	ndmp_config_get_auth_attr_request *request;
	ndmp_config_get_auth_attr_reply reply = { 0 };

	request = (ndmp_config_get_auth_attr_request *)body;

	reply.server_attr.auth_type = request->auth_type;

	switch (request->auth_type) {
	case NDMP_AUTH_TEXT:
		break;
	case NDMP_AUTH_MD5:
		(void) memcpy(reply.server_attr.ndmp_auth_attr_u.challenge,
		    session->ns_challenge, MD5_CHALLENGE_SIZE);
		break;
	case NDMP_AUTH_NONE:
		/* FALL THROUGH */
	default:
		ndmp_log(session, LOG_ERR, "invalid authentication type, "
		    "must be MD5 or cleartext");
		reply.error = NDMP_ILLEGAL_ARGS_ERR;
		break;
	}

	ndmp_send_reply(session, (void *) &reply);
}

/*
 * Get information about the supported backup types.
 */
/*ARGSUSED*/
void
ndmp_config_get_butype_info_v3(ndmp_session_t *session, void *body)
{
	ndmp_server_conf_t *conf = session->ns_server->ns_conf;
	ndmp_config_get_butype_info_reply_v3 reply = { 0 };
	ndmp_butype_info *info;
	int i, ntypes;
	int envsize;
	ndmp_pval *envp;

	for (ntypes = 0; conf->ns_types[ntypes] != NULL; ntypes++)
		continue;

	if ((info = ndmp_malloc(session,
	    ntypes * sizeof (ndmp_butype_info))) == NULL) {
		reply.error = NDMP_NO_MEM_ERR;
		ndmp_send_reply(session, &reply);
		return;
	}

	for (i = 0; i < ntypes; i++) {
		envp = (ndmp_pval *)conf->ns_get_backup_env(
		    conf->ns_types[i], session->ns_version);

		for (envsize = 0; envp[envsize].name != NULL; envsize++)
			;

		info[i].butype_name = (char *)conf->ns_types[i];
		info[i].attrs = conf->ns_get_backup_attrs(conf->ns_types[i]);
		/*
		 * This handler is shared by both the V3 and V4
		 * implementations; the only difference is the set of suported
		 * attributes.
		 */
		if (session->ns_version == NDMPV3) {
			info[i].attrs &= ~(NDMP_BUTYPE_BACKUP_FH_FILE |
			    NDMP_BUTYPE_BACKUP_FH_DIR |
			    NDMP_BUTYPE_RECOVER_FILEHIST |
			    NDMP_BUTYPE_RECOVER_FH_FILE |
			    NDMP_BUTYPE_RECOVER_FH_DIR);
		}
		info[i].default_env.default_env_len = envsize;
		info[i].default_env.default_env_val = envp;
	}

	reply.butype_info.butype_info_len = ntypes;
	reply.butype_info.butype_info_val = info;

	ndmp_send_reply(session, &reply);

	free(info);
}

/*
 * Returns a list of supported data connection types.  If the server has been
 * configured with local tape support, then we support both TCP and LOCAL
 * addresses.  Otherwise, we just support TCP connection types.  We don't
 * support IPC addresses.
 */
/*ARGSUSED*/
void
ndmp_config_get_connection_type_v3(ndmp_session_t *session, void *body)
{
	ndmp_config_get_connection_type_reply_v3 reply = { 0 };
	ndmp_addr_type addr_types[2];

	addr_types[0] = NDMP_ADDR_TCP;
	addr_types[1] = NDMP_ADDR_LOCAL;
	reply.addr_types.addr_types_val = addr_types;
	if (ndmp_get_prop_boolean(session, NDMP_LOCAL_TAPE))
		reply.addr_types.addr_types_len = 2;
	else
		reply.addr_types.addr_types_len = 1;

	ndmp_send_reply(session, &reply);
}

/*
 * Get information about currently mounted filesystems.
 */
/*ARGSUSED*/
void
ndmp_config_get_fs_info_v3(ndmp_session_t *session, void *body)
{
	ndmp_config_get_fs_info_reply_v3 reply = { 0 };
	ndmp_fs_info_v3 *fsip;
	int i, j;

	session->ns_fsinfo_alloc = 16;
	session->ns_fsinfo_count = 0;
	if ((session->ns_fsinfo = ndmp_malloc(session,
	    session->ns_fsinfo_alloc * sizeof (ndmp_fs_info_v3))) == NULL) {
		reply.error = NDMP_NO_MEM_ERR;
	} else {
		reply.error = session->ns_server->ns_conf->ns_list_fs(session);
	}

	if (reply.error == 0) {
		reply.fs_info.fs_info_len = session->ns_fsinfo_count;
		reply.fs_info.fs_info_val = session->ns_fsinfo;
	}

	ndmp_send_reply(session, &reply);

	for (i = 0; i < session->ns_fsinfo_alloc; i++) {
		fsip = &session->ns_fsinfo[i];
		free(fsip->fs_logical_device);
		free(fsip->fs_type);
		free(fsip->fs_status);
		for (j = 0; j < fsip->fs_env.fs_env_len; j++) {
			free(fsip->fs_env.fs_env_val[j].name);
			free(fsip->fs_env.fs_env_val[j].value);
		}
		free(fsip->fs_env.fs_env_val);
	}

	free(session->ns_fsinfo);
	session->ns_fsinfo = NULL;
	session->ns_fsinfo_alloc = session->ns_fsinfo_count = 0;
}

/*
 * Common function shared by GET_SCSI_INFO and GET_TAPE_INFO.
 */
static void
ndmp_common_scsi_info(ndmp_session_t *session, int type)
{
	ndmp_config_get_scsi_info_reply_v3 reply = { 0 };
	ndmp_device_info_v3 *sip, *sip_base;
	ndmp_device_capability_v3 *dcp;
	int count;
	scsi_device_t *sdp;
	ndmp_pval *envp;
	ndmp_pval *envp_head;

	for (sdp = session->ns_server->ns_scsi_devices, count = 0;
	    sdp != NULL; sdp = sdp->sd_next, count++)
		continue;

	if (!ndmp_get_prop_boolean(session, NDMP_LOCAL_TAPE) || count == 0) {
		reply.scsi_info.scsi_info_len = 0;
		ndmp_send_reply(session, &reply);
		return;
	}

	sip_base = sip = alloca(sizeof (ndmp_device_info_v3) * count);
	dcp = alloca(sizeof (ndmp_device_capability_v3) * count);
	envp = alloca(sizeof (ndmp_pval) * count * 3);

	for (sdp = session->ns_server->ns_scsi_devices, count = 0;
	    sdp != NULL; sdp = sdp->sd_next) {

		if (sdp->sd_type != type)
			continue;

		/*
		 * Don't report dead links.
		 */
		if ((access(sdp->sd_name, F_OK) == -1) && (errno == ENOENT))
			continue;

		count++;

		ndmp_debug(session, "model \"%s\" dev \"%s\"", sdp->sd_id,
		    sdp->sd_name);

		envp_head = envp;
		NDMP_SETENV(envp, "SERIAL_NUMBER", sdp->sd_serial);
		NDMP_SETENV(envp, "WORLD_WIDE_NAME", sdp->sd_wwn);
		if (type == DTYPE_SEQUENTIAL)
			NDMP_SETENV(envp, "EXECUTE_CDB", "b");

		sip->model = sdp->sd_id;
		sip->caplist.caplist_len = 1;
		sip->caplist.caplist_val = dcp;
		dcp->device = sdp->sd_name;

		dcp->attr = 0;
		dcp->capability.capability_len =
		    (type == DTYPE_SEQUENTIAL) ? 3 : 2;
		dcp->capability.capability_val = envp_head;
		sip++;
		dcp++;
	}

	reply.scsi_info.scsi_info_len = count;
	reply.scsi_info.scsi_info_val = sip_base;

	ndmp_send_reply(session, &reply);
}

/*
 * Return information about all SCSI tape control devices on the system.
 */
/*ARGSUSED*/
void
ndmp_config_get_scsi_info_v3(ndmp_session_t *session, void *body)
{
	ndmp_common_scsi_info(session, DTYPE_CHANGER);
}

/*
 * Return information about local tape devices.
 */
/*ARGSUSED*/
void
ndmp_config_get_tape_info_v3(ndmp_session_t *session, void *body)
{
	ndmp_common_scsi_info(session, DTYPE_SEQUENTIAL);
}

/*
 * Return information about the NDMP server instance.  This is disinct from the
 * host information returned by GET HOST INFO.
 */
/*ARGSUSED*/
void
ndmp_config_get_server_info_v3(ndmp_session_t *session, void *body)
{
	ndmp_server_conf_t *conf = session->ns_server->ns_conf;
	ndmp_config_get_server_info_reply_v3 reply = { 0 };
	ndmp_auth_type auth_types[2];

	if (session->ns_authorized ||
	    session->ns_version != NDMPV4) {
		reply.vendor_name = (char *)conf->ns_vendor;
		reply.product_name = (char *)conf->ns_product;
		reply.revision_number = (char *)conf->ns_revision;
	} else {
		reply.vendor_name = "\0";
		reply.product_name = "\0";
		reply.revision_number = "\0";
	}

	ndmp_debug(session, "vendor \"%s\", product \"%s\" rev \"%s\"",
	    reply.vendor_name, reply.product_name, reply.revision_number);

	auth_types[0] = NDMP_AUTH_TEXT;
	auth_types[1] = NDMP_AUTH_MD5;
	reply.auth_type.auth_type_len = sizeof (auth_types) /
	    sizeof (auth_types[0]);
	reply.auth_type.auth_type_val = auth_types;

	ndmp_send_reply(session, &reply);
}

/*
 * Get information about supported exctension.  We don't currently support any
 * extensions.
 */
/*ARGSUSED*/
void
ndmp_config_get_ext_list_v4(ndmp_session_t *session, void *body)
{
	ndmp_config_get_ext_list_reply_v4 reply = { 0 };

	if (!session->ns_set_ext_list)
		reply.error = NDMP_EXT_DANDN_ILLEGAL_ERR;

	reply.class_list.class_list_val = NULL;
	reply.class_list.class_list_len = 0;

	ndmp_send_reply(session, &reply);
}

/*
 * Set information about supported extentions.  We don't currently support any
 * extensions.
 */
/*ARGSUSED*/
void
ndmp_config_set_ext_list_v4(ndmp_session_t *session, void *body)
{
	ndmp_config_set_ext_list_reply_v4 reply = { 0 };

	if (session->ns_set_ext_list) {
		reply.error = NDMP_EXT_DANDN_ILLEGAL_ERR;
	} else {
		session->ns_set_ext_list = B_TRUE;
		reply.error = NDMP_VERSION_NOT_SUPPORTED_ERR;
	}

	ndmp_send_reply(session, &reply);
}
