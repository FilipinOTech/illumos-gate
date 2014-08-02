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
 * Dispatch function for SMB2_SESSION_SETUP
 */

#include <smbsrv/smb2_kproto.h>

smb_sdrc_t
smb2_session_setup(smb_request_t *sr)
{
	smb_arg_sessionsetup_t	*sinfo;
	smb_session_t *s = sr->session;
	uint16_t StructureSize;
	uint8_t  Flags;
	uint8_t  SecurityMode;
	uint32_t Channel;
	uint16_t SecBufOffset;
	uint16_t SecBufLength;
	uint64_t PrevSessionId;
	uint16_t SessionFlags;
	uint32_t status;
	int skip;
	int rc = 0;

	sinfo = smb_srm_zalloc(sr, sizeof (smb_arg_sessionsetup_t));
	sr->sr_ssetup = sinfo;

	rc = smb_mbc_decodef(
	    &sr->smb_data, "wbbllwwq",
	    &StructureSize,
	    &Flags,
	    &SecurityMode,
	    &s->capabilities,
	    &Channel,
	    &SecBufOffset,
	    &SecBufLength,
	    &PrevSessionId);
	if (rc)
		return (SDRC_ERROR);

	/*
	 * We're normally positioned at the security buffer now,
	 * but there could be some padding before it.
	 */
	skip = (SecBufOffset + sr->smb2_cmd_hdr) -
	    sr->smb_data.chain_offset;
	if (skip < 0)
		return (SDRC_ERROR);
	if (skip > 0)
		(void) smb_mbc_decodef(&sr->smb_data, "#.", skip);

	/*
	 * Get the security buffer
	 */
	sinfo->ssi_iseclen = SecBufLength;
	sinfo->ssi_isecblob = smb_srm_zalloc(sr, sinfo->ssi_iseclen);
	rc = smb_mbc_decodef(&sr->smb_data, "#c",
	    sinfo->ssi_iseclen, sinfo->ssi_isecblob);
	if (rc)
		return (SDRC_ERROR);

	/*
	 * The real auth. work happens in here.
	 */
	status = smb_authenticate_ext(sr);

	SecBufOffset = SMB2_HDR_SIZE + 8;
	SecBufLength = sinfo->ssi_oseclen;
	SessionFlags = 0;

	switch (status) {

	case NT_STATUS_SUCCESS:
		if (sr->uid_user->u_flags & SMB_USER_FLAG_GUEST)
			SessionFlags |= SMB2_SESSION_FLAG_IS_GUEST;
		if (sr->uid_user->u_flags & SMB_USER_FLAG_ANON)
			SessionFlags |= SMB2_SESSION_FLAG_IS_NULL;
		break;

	/*
	 * This is not really an error, but tells the client
	 * it should send another session setup request.
	 * Not smb2_put_error because we send a payload.
	 */
	case NT_STATUS_MORE_PROCESSING_REQUIRED:
		sr->smb2_status = status;
		break;

	default:
		SecBufLength = 0;
		sr->smb2_status = status;
		break;
	}

	/*
	 * SMB2 Session Setup reply
	 */

	rc = smb_mbc_encodef(
	    &sr->reply,
	    "wwww#c",
	    9,	/* StructSize */	/* w */
	    SessionFlags,		/* w */
	    SecBufOffset,		/* w */
	    SecBufLength,		/* w */
	    SecBufLength,		/* # */
	    sinfo->ssi_osecblob);	/* c */
	if (rc)
		return (SDRC_ERROR);

	return (SDRC_SUCCESS);
}
