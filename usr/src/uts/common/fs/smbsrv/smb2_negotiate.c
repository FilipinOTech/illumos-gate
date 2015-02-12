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
 * Copyright 2014 Nexenta Systems, Inc.  All rights reserved.
 */

/*
 * Dispatch function for SMB2_NEGOTIATE
 */

#include <smbsrv/smb2_kproto.h>
#include <smbsrv/smb2.h>

static int smb2_negotiate_common(smb_request_t *, uint16_t);

uint32_t smb2srv_capabilities =
	SMB2_CAP_DFS; /* XXX: more to come */

/* No, these should not be easy to "tune". */
uint32_t smb2_tcp_sndbuf = (1<<20);
uint32_t smb2_tcp_rcvbuf = (1<<20);
uint32_t smb2_max_rwsize = (1<<16);
uint32_t smb2_max_trans  = (1<<16);

/*
 * Which SMB2 dialects do we support?
 */
static uint16_t smb2_dialects[] = {
	0x202,	/* SMB 2.002 */
	0x210,	/* SMB 2.1 */
};
static uint16_t smb2_ndialects = 2;

static boolean_t
smb2_supported_dialect(uint16_t dialect)
{
	int i;
	for (i = 0; i < smb2_ndialects; i++)
		if (dialect == smb2_dialects[i])
			return (B_TRUE);
	return (B_FALSE);
}

/*
 * Helper for the (SMB1) smb_com_negotiate().  This is the
 * very unusual protocol interaction where an SMB1 negotiate
 * gets an SMB2 negotiate response.  This is the normal way
 * clients first find out if the server supports SMB2.
 *
 * Note: This sends an SMB2 reply _itself_ and then returns
 * SDRC_NO_REPLY so the caller will not send an SMB1 reply.
 * Also, this is called directly from the reader thread, so
 * we know this is the only thread using this session.
 *
 * The caller frees this request.
 */
smb_sdrc_t
smb1_negotiate_smb2(smb_request_t *sr)
{
	smb_session_t *s = sr->session;
	smb_arg_negotiate_t *negprot = sr->sr_negprot;
	uint16_t smb2_version;
	uint16_t secmode2;
	int rc;

	/*
	 * Note: In the SMB1 negotiate command handler, we
	 * agreed with one of the SMB2 dialects.  If that
	 * dialect was "SMB 2.002", we'll respond here with
	 * version 0x202 and negotiation is done.  If that
	 * dialect was "SMB 2.???", we'll respond here with
	 * the "wildcard" version 0x2FF, and the client will
	 * come back with an SMB2 negotiate.
	 */
	switch (negprot->ni_dialect) {
	case DIALECT_SMB2002:	/* SMB 2.002 (a.k.a. SMB2.0) */
		smb2_version = 0x202;
		s->dialect = smb2_version;
		s->s_state = SMB_SESSION_STATE_NEGOTIATED;
		/* Allow normal SMB2 requests now. */
		s->newrq_func = smb2sr_newrq;

		/*
		 * Translate SMB1 sec. mode to SMB2.
		 */
		secmode2 = 0;
		if (s->secmode & NEGOTIATE_SECURITY_SIGNATURES_ENABLED)
			secmode2 |= SMB2_NEGOTIATE_SIGNING_ENABLED;
		if (s->secmode & NEGOTIATE_SECURITY_SIGNATURES_REQUIRED)
			secmode2 |= SMB2_NEGOTIATE_SIGNING_REQUIRED;
		s->secmode = secmode2;
		break;
	case DIALECT_SMB2XXX:	/* SMB 2.??? (wildcard vers) */
		/*
		 * Expecting an SMB2 negotiate next, so
		 * keep the initial s->newrq_func.
		 */
		smb2_version = 0x2FF;
		s->secmode = 0;
		break;
	default:
		return (SDRC_DROP_VC);
	}

	/*
	 * We did not decode an SMB2 header, so make sure
	 * the SMB2 header fields are initialized.
	 * (Most are zero from smb_request_alloc.)
	 * Also, the SMB1 common dispatch code reserved space
	 * for an SMB1 header, which we need to undo here.
	 */
	sr->smb2_reply_hdr = sr->reply.chain_offset = 0;
	sr->smb2_cmd_code = SMB2_NEGOTIATE;

	rc = smb2_negotiate_common(sr, smb2_version);
	if (rc != 0)
		return (SDRC_DROP_VC);

	return (SDRC_NO_REPLY);
}

/*
 * SMB2 Negotiate gets special handling.  This is called directly by
 * the reader thread (see smbsr_newrq_initial) with what _should_ be
 * an SMB2 Negotiate.  Only the "\feSMB" header has been checked
 * when this is called, so this needs to check the SMB command,
 * if it's Negotiate execute it, then send the reply, etc.
 *
 * Since this is called directly from the reader thread, we
 * know this is the only thread currently using this session.
 * This has to duplicate some of what smb2sr_work does as a
 * result of bypassing the normal dispatch mechanism.
 *
 * The caller always frees this request.
 */
int
smb2_newrq_negotiate(smb_request_t *sr)
{
	smb_session_t *s = sr->session;
	int i, rc;
	uint16_t struct_size;
	uint16_t best_dialect;
	uint16_t dialect_cnt;
	uint16_t cl_dialects[8];

	sr->smb2_cmd_hdr = sr->command.chain_offset;
	rc = smb2_decode_header(sr);
	if (rc != 0)
		return (rc);

	if ((sr->smb2_cmd_code != SMB2_NEGOTIATE) ||
	    (sr->smb2_next_command != 0))
		return (SDRC_DROP_VC);

	/*
	 * Conditionally enable SMB2
	 */
	if (sr->sr_server->sv_cfg.skc_smb2_enable == 0)
		return (SDRC_DROP_VC);

	/*
	 * Decode SMB2 Negotiate (fixed-size part)
	 */
	rc = smb_mbc_decodef(
	    &sr->command, "www..l16.8.",
	    &struct_size,	/* w */
	    &dialect_cnt,	/* w */
	    &s->secmode,	/* w */
	    /* reserved 	(..) */
	    &s->capabilities);	/* l */
	    /* clnt_uuid	 16. */
	    /* start_time	  8. */
	if (rc != 0)
		return (rc);
	if (struct_size != 36 || dialect_cnt > 8)
		return (SDRC_DROP_VC);

	/*
	 * Decode SMB2 Negotiate (variable part)
	 */
	rc = smb_mbc_decodef(&sr->command,
	    "#w", dialect_cnt, cl_dialects);
	if (rc != 0)
		return (SDRC_DROP_VC);

	/*
	 * Choose a dialect (SMB2 version).
	 */
	best_dialect = 0;
	for (i = 0; i < dialect_cnt; i++)
		if (smb2_supported_dialect(cl_dialects[i]) &&
		    best_dialect < cl_dialects[i])
			best_dialect = cl_dialects[i];
	if (best_dialect == 0)
		return (SDRC_DROP_VC);
	s->dialect = best_dialect;

	/* Allow normal SMB2 requests now. */
	s->s_state = SMB_SESSION_STATE_NEGOTIATED;
	s->newrq_func = smb2sr_newrq;

	rc = smb2_negotiate_common(sr, best_dialect);
	if (rc != 0)
		return (SDRC_DROP_VC);

	return (0);
}

/*
 * Common parts of SMB2 Negotiate, used for both the
 * SMB1-to-SMB2 style, and straight SMB2 style.
 * Do negotiation decisions, encode, send the reply.
 */
static int
smb2_negotiate_common(smb_request_t *sr, uint16_t dialect)
{
	static timestruc_t boot_time = { 1261440000L, 0 };
	timestruc_t server_time;
	smb_session_t *s = sr->session;
	int rc;
	uint16_t secmode;

	/*
	 * Negotiation itself
	 */
	secmode = SMB2_NEGOTIATE_SIGNING_ENABLED;
	if (sr->sr_cfg->skc_signing_required)
		secmode |= SMB2_NEGOTIATE_SIGNING_REQUIRED;
	s->secmode = secmode;
	s->cmd_max_bytes = smb2_tcp_rcvbuf;
	s->reply_max_bytes = smb2_tcp_sndbuf;
	(void) microtime(&server_time);

	/*
	 * SMB2 header
	 */
	sr->smb2_status = 0;
	sr->smb2_credit_response = s->s_cfg.skc_initial_credits;
	sr->smb2_hdr_flags = SMB2_FLAGS_SERVER_TO_REDIR;
	(void) smb2_encode_header(sr, B_FALSE);

	/*
	 * SMB2 negotiate reply
	 */
	rc = smb_mbc_encodef(
	    &sr->reply,
	    "wwww#cllllTTwwl#c",
	    65,	/* StructSize */	/* w */
	    s->secmode,			/* w */
	    dialect,			/* w */
	    0, /* reserved */		/* w */
	    UUID_LEN,			/* # */
	    &s->s_cfg.skc_machine_uuid, /* c */
	    smb2srv_capabilities,	/* l */
	    smb2_max_trans,		/* l */
	    smb2_max_rwsize,		/* l */
	    smb2_max_rwsize,		/* l */
	    &server_time,		/* T */
	    &boot_time,			/* T */
	    128, /* SecBufOff */	/* w */
	    sr->sr_cfg->skc_negtok_len,	/* w */
	    0,	/* reserved */		/* l */
	    sr->sr_cfg->skc_negtok_len,	/* # */
	    sr->sr_cfg->skc_negtok);	/* c */

	smb2_send_reply(sr);

	(void) ksocket_setsockopt(s->sock, SOL_SOCKET,
	    SO_SNDBUF, (const void *)&smb2_tcp_sndbuf,
	    sizeof (smb2_tcp_sndbuf), CRED());
	(void) ksocket_setsockopt(s->sock, SOL_SOCKET,
	    SO_RCVBUF, (const void *)&smb2_tcp_rcvbuf,
	    sizeof (smb2_tcp_rcvbuf), CRED());

	return (rc);
}

/*
 * SMB2 Dispatch table handler, which will run if we see an
 * SMB2_NEGOTIATE after the initial negotiation is done.
 * That would be a protocol error.
 */
smb_sdrc_t
smb2_negotiate(smb_request_t *sr)
{
	sr->smb2_status = NT_STATUS_INVALID_PARAMETER;
	return (SDRC_ERROR);
}

/*
 * VALIDATE_NEGOTIATE_INFO [MS-SMB2] 2.2.32.6
 */
uint32_t
smb2_fsctl_vneginfo(smb_request_t *sr, smb_fsctl_t *fsctl)
{
	smb_session_t *s = sr->session;
	int rc;

	/*
	 * We're supposed to parse the client's
	 * VALIDATE_NEGOTIATE_INFO here, but it
	 * appears to be unnecessary.
	 */

	rc = smb_mbc_encodef(
	    fsctl->out_mbc, "l#cww",
	    smb2srv_capabilities,	/* l */
	    UUID_LEN,			/* # */
	    &s->s_cfg.skc_machine_uuid, /* c */
	    s->secmode,			/* w */
	    s->dialect);		/* w */
	if (rc)
		return (NT_STATUS_INTERNAL_ERROR);

	return (0);
}
