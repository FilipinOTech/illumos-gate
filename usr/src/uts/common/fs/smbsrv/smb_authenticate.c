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

/*
 * Authentication support for SMB session setup
 */

#include <sys/types.h>
#include <sys/sid.h>
#include <sys/priv_names.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <smbsrv/smb_idmap.h>
#include <smbsrv/smb_kproto.h>
#include <smbsrv/smb_token.h>

static int smb_authsock_open(ksocket_t *);
static int smb_authsock_send(ksocket_t, void *, size_t);
static int smb_authsock_recv(ksocket_t, void *, size_t);
static int smb_authsock_sendrecv(ksocket_t, smb_lsa_msg_hdr_t *hdr,
				void *sndbuf, void **recvbuf);
/* int smb_authsock_close(ksocket_t); kproto.h */

static int smb_auth_do_clinfo(smb_request_t *);
static uint32_t smb_auth_do_oldreq(smb_request_t *);
static uint32_t smb_auth_get_token(smb_request_t *);
static uint32_t smb_priv_xlate(smb_token_t *);

/*
 * Handle old-style session setup (non-extended security)
 *
 * The user information is passed to smbd for authentication.
 * If smbd can authenticate the user an access token is returned and we
 * generate a cred and new user based on the token.
 */
int
smb_authenticate_old(smb_request_t *sr)
{
	smb_user_t	*user = NULL;
	ksocket_t	so = NULL;
	uint32_t	status;
	int		rc;

	user = smb_user_new(sr->session);
	if (user == NULL)
		return (NT_STATUS_TOO_MANY_SESSIONS);

	/* user cleanup in smb_request_free */
	sr->uid_user = user;
	sr->smb_uid = user->u_uid;

	/*
	 * Open a connection to the local logon service.
	 * If we can't, it's probably not running.
	 */
	rc = smb_authsock_open(&so);
	if (rc != 0) {
		cmn_err(CE_NOTE, "smb_authsock_open, rc=%d", rc);
		return (NT_STATUS_NETLOGON_NOT_STARTED);
	}

	/* Note: so cleanup in smb_user_logon or logoff */
	user->u_authsock = so;

	/*
	 * Tell the auth. svc who this client is.  This is our first
	 * request on the socket.  If this gets ECONNRESET or similar,
	 * the auth. svc. probably closed the socket because there are
	 * too many concurrent authentications happening, so return an
	 * error indicating the auth. service is busy.
	 */
	rc = smb_auth_do_clinfo(sr);
	if (rc != 0) {
		cmn_err(CE_NOTE, "smb_auth_do_clinfo, rc=%d", rc);
		return (NT_STATUS_NO_LOGON_SERVERS);
	}

	status = smb_auth_do_oldreq(sr);
	if (status != 0)
		return (status);

	/*
	 * Get the final auth. token.
	 */
	status = smb_auth_get_token(sr);
	return (status);
}

/*
 * Build an authentication request message and
 * send it to the local logon service.
 */
static uint32_t
smb_auth_do_oldreq(smb_request_t *sr)
{
	smb_lsa_msg_hdr_t	msg_hdr;
	smb_logon_t	user_info;
	XDR		xdrs;
	smb_arg_sessionsetup_t *sinfo = sr->sr_ssetup;
	smb_user_t	*user = sr->uid_user;
	ksocket_t	so = user->u_authsock;
	void		*sbuf = NULL;
	void		*rbuf = NULL;
	uint32_t	slen = 0;
	uint32_t	rlen = 0;
	uint32_t	status = NT_STATUS_INTERNAL_ERROR;
	int		rc;

	bzero(&user_info, sizeof (smb_logon_t));

	user_info.lg_level = NETR_NETWORK_LOGON;
	user_info.lg_username = sinfo->ssi_user;
	user_info.lg_domain = sinfo->ssi_domain;
	user_info.lg_workstation = sr->session->workstation;
	user_info.lg_clnt_ipaddr = sr->session->ipaddr;
	user_info.lg_local_ipaddr = sr->session->local_ipaddr;
	user_info.lg_local_port = sr->session->s_local_port;
	user_info.lg_challenge_key.val = sr->session->challenge_key;
	user_info.lg_challenge_key.len = sr->session->challenge_len;
	user_info.lg_nt_password.val = sinfo->ssi_ntpwd;
	user_info.lg_nt_password.len = sinfo->ssi_ntpwlen;
	user_info.lg_lm_password.val = sinfo->ssi_lmpwd;
	user_info.lg_lm_password.len = sinfo->ssi_lmpwlen;
	user_info.lg_native_os = sr->session->native_os;
	user_info.lg_native_lm = sr->session->native_lm;
	/* lg_flags? */

	slen = xdr_sizeof(smb_logon_xdr, &user_info);
	sbuf = kmem_alloc(slen, KM_SLEEP);
	xdrmem_create(&xdrs, sbuf, slen, XDR_ENCODE);
	rc = smb_logon_xdr(&xdrs, &user_info);
	xdr_destroy(&xdrs);
	if (!rc)
		goto out;

	msg_hdr.lmh_msgtype = LSA_MTYPE_OLDREQ;
	msg_hdr.lmh_msglen = slen;
	rc = smb_authsock_sendrecv(so, &msg_hdr, sbuf, &rbuf);
	if (rc)
		goto out;
	rlen = msg_hdr.lmh_msglen;
	kmem_free(sbuf, slen);
	sbuf = NULL;

	/*
	 * Decode the response message.
	 */
	switch (msg_hdr.lmh_msgtype) {

	case LSA_MTYPE_ERROR: {
		smb_lsa_eresp_t *eresp = rbuf;
		if (rlen != sizeof (*eresp))
			goto out;
		status = eresp->ler_ntstatus;
		goto out;
	}

	case LSA_MTYPE_OK:
		status = 0;
		break;

	/*  Bogus message type */
	default:
		break;
	}

out:
	if (rbuf != NULL)
		kmem_free(rbuf, rlen);
	if (sbuf != NULL)
		kmem_free(sbuf, slen);

	return (status);
}

/*
 * Handle new-style (extended security) session setup.
 * Returns zero: success, non-zero: error (value not used)
 *
 * Note that this style uses a sequence of session setup requests,
 * where the first has SMB UID=0, and subsequent requests in the
 * same authentication sequence have the SMB UID returned for that
 * first request.  We allocate a USER object when the first request
 * in the sequence arrives (SMB_USER_STATE_LOGGING_ON) and use that
 * to maintain state between requests in this sequence.  The state
 * for one sequence includes an AF_UNIX "authsock" connection to the
 * user-space smbd.  The neat part of this is: in smbd, the handler
 * for the server-side of one authsock gets only request specific to
 * one authentication sequence, simplifying it's work immensely.
 * When the authentication sequence is finished, with either success
 * or failure, the local side of the authsock is closed.
 *
 * As with the old-style authentication, if we succeed, then the
 * last message from smbd will be an smb_token_t encoding the
 * information about the new user.
 *
 * Outline:
 * (a) On the first request (UID==0) create a USER object,
 *     and on subsequent requests, find USER by SMB UID.
 * (b) Send message / recv. response as above,
 * (c) If response says "we're done", close authsock
 *     (both success and failure must close authsock)
 */
int
smb_authenticate_ext(smb_request_t *sr)
{
	smb_arg_sessionsetup_t *sinfo = sr->sr_ssetup;
	ksocket_t	so = NULL;
	smb_user_t	*user = NULL;
	void		*rbuf = NULL;
	uint32_t	rlen = 0;
	smb_lsa_msg_hdr_t	msg_hdr;
	uint32_t	status = NT_STATUS_INTERNAL_ERROR;
	int		rc;

	ASSERT(sr->uid_user == NULL);

	/*
	 * On the first request (UID==0) create a USER object.
	 * On subsequent requests (UID!=0) find the USER object.
	 * Either way, sr->uid_user is set, so our ref. on the
	 * user object is dropped during normal cleanup work
	 * for the smb_request (sr).  Ditto u_authsock.
	 */
	if (sr->smb_uid == 0) {
		msg_hdr.lmh_msgtype = LSA_MTYPE_ESFIRST;
		user = smb_user_new(sr->session);
		if (user == NULL)
			return (NT_STATUS_TOO_MANY_SESSIONS);

		/* user cleanup in smb_request_free */
		sr->uid_user = user;
		sr->smb_uid = user->u_uid;

		/*
		 * Open a connection to the local logon service.
		 * If we can't, it's probably not running.
		 */
		rc = smb_authsock_open(&so);
		if (rc != 0) {
			cmn_err(CE_NOTE, "smb_authsock_open, rc=%d", rc);
			return (NT_STATUS_NETLOGON_NOT_STARTED);
		}

		/* Note: so cleanup in smb_user_logon or logoff. */
		user->u_authsock = so;

		/*
		 * Tell the auth. svc who this client is.  This is our
		 * first request on the socket.  If this gets ECONNRESET
		 * or similar, the auth. svc. probably closed the socket
		 * because there are too many concurrent authentications
		 * happening, so return an error indicating the auth.
		 * service is busy.
		 */
		rc = smb_auth_do_clinfo(sr);
		if (rc != 0) {
			cmn_err(CE_NOTE, "smb_auth_do_clinfo, rc=%d", rc);
			return (NT_STATUS_NO_LOGON_SERVERS);
		}
	} else {
		msg_hdr.lmh_msgtype = LSA_MTYPE_ESNEXT;
		user = smb_session_lookup_uid_st(sr->session,
		    sr->smb_uid, SMB_USER_STATE_LOGGING_ON);
		if (user == NULL)
			return (NT_STATUS_USER_SESSION_DELETED);

		/* user cleanup in smb_request_free */
		sr->uid_user = user;
		so = user->u_authsock;
	}

	/*
	 * Wrap the "security blob" with our header
	 * (LSA_MTYPE_ESFIRST or LSA_MTYPE_ESNEXT)
	 * and send it up the authsock with either
	 */
	msg_hdr.lmh_msglen = sinfo->ssi_iseclen;
	rc = smb_authsock_sendrecv(so, &msg_hdr,
	    sinfo->ssi_isecblob, &rbuf);
	if (rc)
		goto out;
	rlen = msg_hdr.lmh_msglen;

	/*
	 * Decode the response message.
	 * Note: allocated rbuf
	 */
	switch (msg_hdr.lmh_msgtype) {

	case LSA_MTYPE_ERROR: {
		/*
		 * Authentication failed.  Return the error
		 * provided in the reply message.
		 */
		smb_lsa_eresp_t *eresp = rbuf;
		if (rlen != sizeof (*eresp))
			goto out;
		status = eresp->ler_ntstatus;
		break;
	}

	case LSA_MTYPE_ES_CONT:
		sinfo->ssi_oseclen = (uint16_t)rlen;
		sinfo->ssi_osecblob = smb_srm_alloc(sr, sinfo->ssi_oseclen);
		bcopy(rbuf, sinfo->ssi_osecblob, sinfo->ssi_oseclen);
		/*
		 * This is not really an error, but tells the client
		 * it should send another session setup request.
		 */
		status = NT_STATUS_MORE_PROCESSING_REQUIRED;
		break;

	case LSA_MTYPE_ES_DONE:
		sinfo->ssi_oseclen = (uint16_t)rlen;
		sinfo->ssi_osecblob = smb_srm_alloc(sr, sinfo->ssi_oseclen);
		bcopy(rbuf, sinfo->ssi_osecblob, sinfo->ssi_oseclen);
		/*
		 * Get the final auth. token.
		 */
		status = smb_auth_get_token(sr);
		break;

	/*  Bogus message type */
	default:
		break;
	}

out:
	if (rbuf != NULL)
		kmem_free(rbuf, rlen);

	return (status);
}

/*
 * Send the "client info" up to the auth service.
 */
static int
smb_auth_do_clinfo(smb_request_t *sr)
{
	smb_lsa_msg_hdr_t msg_hdr;
	smb_lsa_clinfo_t clinfo;
	smb_user_t *user = sr->uid_user;
	ksocket_t so = user->u_authsock;
	void *rbuf = NULL;
	int rc;

	/*
	 * Send a message with info. about the client
	 * (IP address, etc) and wait for an ACK.
	 */
	msg_hdr.lmh_msgtype = LSA_MTYPE_CLINFO;
	msg_hdr.lmh_msglen = sizeof (clinfo);
	clinfo.lci_clnt_ipaddr = sr->session->ipaddr;
	(void) memcpy(clinfo.lci_challenge_key,
	    sr->session->challenge_key,
	    sizeof (clinfo.lci_challenge_key));
	rc = smb_authsock_sendrecv(so, &msg_hdr, &clinfo, &rbuf);
	/* We don't use this response. */
	if (rbuf != NULL) {
		kmem_free(rbuf, msg_hdr.lmh_msglen);
		rbuf = NULL;
	}

	return (rc);
}

/*
 * After a successful authentication, ask the authsvc to
 * send us the authentication token.
 */
static uint32_t
smb_auth_get_token(smb_request_t *sr)
{
	smb_lsa_msg_hdr_t msg_hdr;
	XDR		xdrs;
	smb_arg_sessionsetup_t *sinfo = sr->sr_ssetup;
	smb_user_t	*user = sr->uid_user;
	ksocket_t	so = user->u_authsock;
	smb_token_t	*token = NULL;
	cred_t		*cr = NULL;
	void		*rbuf = NULL;
	uint32_t	rlen = 0;
	uint32_t	privileges;
	uint32_t	status = NT_STATUS_INTERNAL_ERROR;
	int		rc;

	msg_hdr.lmh_msgtype = LSA_MTYPE_GETTOK;
	msg_hdr.lmh_msglen = 0;

	rc = smb_authsock_sendrecv(so, &msg_hdr, NULL, &rbuf);
	if (rc)
		goto errout;

	rlen = msg_hdr.lmh_msglen;
	if (msg_hdr.lmh_msgtype == LSA_MTYPE_ERROR) {
		smb_lsa_eresp_t *eresp = rbuf;
		if (rlen != sizeof (*eresp)) {
			goto errout;
		}
		status = eresp->ler_ntstatus;
		goto errout;
	}

	if (msg_hdr.lmh_msgtype != LSA_MTYPE_TOKEN)
		goto errout;

	/*
	 * Authenticated.  Decode the LSA_MTYPE_TOKEN.
	 */
	xdrmem_create(&xdrs, rbuf, rlen, XDR_DECODE);
	token = kmem_zalloc(sizeof (smb_token_t), KM_SLEEP);
	rc = smb_token_xdr(&xdrs, token);
	xdr_destroy(&xdrs);
	if (!rc)
		goto errout;
	kmem_free(rbuf, rlen);
	rbuf = NULL;

	/*
	 * Setup the logon object.
	 */
	cr = smb_cred_create(token);
	if (cr == NULL)
		goto errout;
	privileges = smb_priv_xlate(token);
	(void) smb_user_logon(user, cr,
	    token->tkn_domain_name, token->tkn_account_name,
	    token->tkn_flags, privileges, token->tkn_audit_sid);
	crfree(cr);

	/*
	 * Save the session key, and (maybe) enable signing.
	 */
	if (token->tkn_session_key) {
		bcopy(token->tkn_session_key, sinfo->ssi_ssnkey,
		    SMB_SSNKEY_LEN);
		if (sr->session->signing.mackey == NULL) {
			if (sr->session->dialect >= 0x200)
				smb2_sign_begin(sr, sinfo);
			else
				smb_sign_begin(sr, sinfo);
		}
	}

	smb_token_free(token);

	sr->user_cr = user->u_cred;
	return (0);

errout:
	if (rbuf != NULL)
		kmem_free(rbuf, rlen);
	if (token != NULL)
		smb_token_free(token);
	return (status);
}

/*
 * Tokens are allocated in the kernel via XDR.
 * Call xdr_free before freeing the token structure.
 */
void
smb_token_free(smb_token_t *token)
{
	if (token != NULL) {
		xdr_free(smb_token_xdr, (char *)token);
		kmem_free(token, sizeof (smb_token_t));
	}
}

/*
 * Convert access token privileges to local definitions.
 */
static uint32_t
smb_priv_xlate(smb_token_t *token)
{
	uint32_t	privileges = 0;

	if (smb_token_query_privilege(token, SE_BACKUP_LUID))
		privileges |= SMB_USER_PRIV_BACKUP;

	if (smb_token_query_privilege(token, SE_RESTORE_LUID))
		privileges |= SMB_USER_PRIV_RESTORE;

	if (smb_token_query_privilege(token, SE_TAKE_OWNERSHIP_LUID))
		privileges |= SMB_USER_PRIV_TAKE_OWNERSHIP;

	if (smb_token_query_privilege(token, SE_SECURITY_LUID))
		privileges |= SMB_USER_PRIV_SECURITY;

	return (privileges);
}

/*
 * Send/recv a request/reply sequence on the auth socket.
 * Returns zero or an errno.
 */
static int
smb_authsock_sendrecv(ksocket_t so, smb_lsa_msg_hdr_t *hdr,
	void *sndbuf, void **recvbuf)
{
	int rc;

	rc = smb_authsock_send(so, hdr, sizeof (*hdr));
	if (rc)
		return (rc);
	if (hdr->lmh_msglen != 0) {
		rc = smb_authsock_send(so, sndbuf, hdr->lmh_msglen);
		if (rc)
			return (rc);
	}

	rc = smb_authsock_recv(so, hdr, sizeof (*hdr));
	if (rc)
		return (rc);
	if (hdr->lmh_msglen == 0) {
		*recvbuf = NULL;
	} else {
		*recvbuf = kmem_alloc(hdr->lmh_msglen, KM_SLEEP);
		rc = smb_authsock_recv(so, *recvbuf, hdr->lmh_msglen);
		if (rc) {
			kmem_free(*recvbuf, hdr->lmh_msglen);
			*recvbuf = NULL;
		}
	}

	return (rc);
}

/*
 * Hope this is interpreted per-zone...
 */
static struct sockaddr_un smbauth_sockname = {
	AF_UNIX, SMB_AUTHSVC_SOCKNAME };

/*
 * Limit how long smb_authsock_sendrecv() will wait for a
 * response from the local authentication service.
 */
struct timeval smb_auth_recv_tmo = { 45, 0 };

/*
 * Also limit the time smb_authsock_sendrecv() will wait
 * trying to send a request to the authentication service.
 */
struct timeval smb_auth_send_tmo = { 15, 0 };

static int
smb_authsock_open(ksocket_t *so)
{
	int rc;

	rc = ksocket_socket(so, AF_UNIX, SOCK_STREAM, 0,
	    KSOCKET_SLEEP, CRED());
	if (rc != 0)
		return (rc);

	/*
	 * Set the send/recv timeouts.
	 */
	rc = ksocket_setsockopt(*so, SOL_SOCKET, SO_SNDTIMEO,
	    &smb_auth_send_tmo, sizeof (smb_auth_send_tmo), CRED());
	if (rc != 0)
		goto errout;
	rc = ksocket_setsockopt(*so, SOL_SOCKET, SO_RCVTIMEO,
	    &smb_auth_recv_tmo, sizeof (smb_auth_recv_tmo), CRED());
	if (rc != 0)
		goto errout;

	/*
	 * Connect to the smbd auth. service.
	 *
	 * Would like to set the connect timeout too, but there's
	 * apparently no easy way to do that for AF_UNIX.
	 */
	rc = ksocket_connect(*so, (struct sockaddr *)&smbauth_sockname,
	    sizeof (smbauth_sockname), CRED());
	if (rc == 0)
		return (0);

errout:
	(void) ksocket_close(*so, CRED());
	*so = NULL;

	return (rc);
}

static int smb_authsock_send(ksocket_t so, void *buf, size_t len)
{
	int rc;
	size_t iocnt = 0;

	rc = ksocket_send(so, buf, len, 0, &iocnt, CRED());
	if (rc == 0 && iocnt != len)
		rc = EIO;

	return (rc);
}

static int smb_authsock_recv(ksocket_t so, void *buf, size_t len)
{
	int rc;
	size_t iocnt = 0;

	rc = ksocket_recv(so, buf, len, MSG_WAITALL, &iocnt, CRED());
	if (rc == 0 && iocnt != len)
		rc = EIO;

	return (rc);
}

int
smb_authsock_close(ksocket_t so)
{

	return (ksocket_close(so, CRED()));
}
