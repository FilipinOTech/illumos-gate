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
 * These routines provide the SMB MAC signing for the SMB2 server.
 * The routines calculate the signature of a SMB message in an mbuf chain.
 *
 * The following table describes the client server
 * signing registry relationship
 *
 *		| Required	| Enabled     | Disabled
 * -------------+---------------+------------ +--------------
 * Required	| Signed	| Signed      | Fail
 * -------------+---------------+-------------+-----------------
 * Enabled	| Signed	| Signed      | Not Signed
 * -------------+---------------+-------------+----------------
 * Disabled	| Fail		| Not Signed  | Not Signed
 */

#include <sys/uio.h>
#include <smbsrv/smb_kproto.h>
#include <smbsrv/msgbuf.h>
#include <sys/isa_defs.h>
#include <sys/byteorder.h>
#define	_SHA2_IMPL /* for SHA256_HMAC_MECH_INFO_TYPE */
#include <sys/sha2.h>

#define	SMB2_SIG_OFFS	48
#define	SMB2_SIG_SIZE	16

#define	SHA256_HMAC_INTS_PER_BLOCK	\
	(SHA256_HMAC_BLOCK_SIZE/sizeof (uint64_t))

typedef struct sha2_hc_ctx {
	SHA2_CTX	hc_icontext;    /* inner SHA2 context */
	SHA2_CTX	hc_ocontext;    /* outer SHA2 context */
} sha2_hc_ctx_t;

static int smb2_hmac_sha256_init(sha2_hc_ctx_t *, uint8_t *, uint_t);
static void smb2_hmac_sha256_update(sha2_hc_ctx_t *, uint8_t *, uint_t);
static void smb2_hmac_sha256_final(sha2_hc_ctx_t *, uint8_t *);

/*
 * smb2_sign_begin
 *
 * Intializes MAC key based on the user session key and
 * NTLM response and store it in the signing structure.
 * This is what begins SMB2 signing.
 */
void
smb2_sign_begin(smb_request_t *sr, smb_arg_sessionsetup_t *sinfo)
{
	smb_session_t *session = sr->session;
	struct smb_sign *sign = &session->signing;

	ASSERT(sign->mackey == NULL);

	/*
	 * Don't turn on signing after anon or guest login.
	 * (Wait for a "real" login.)
	 */
	if (sr->uid_user->u_flags &
	    (SMB_USER_FLAG_GUEST | SMB_USER_FLAG_ANON))
		return;

	/*
	 * With extended security, the MAC key is the same as the
	 * session key.
	 */
	sign->mackey_len = SMB_SSNKEY_LEN;
	sign->mackey = kmem_alloc(sign->mackey_len, KM_SLEEP);
	bcopy(sinfo->ssi_ssnkey, sign->mackey, SMB_SSNKEY_LEN);

	sign->flags = 0;
	if (session->secmode & SMB2_NEGOTIATE_SIGNING_ENABLED) {
		sign->flags |= SMB_SIGNING_ENABLED;
		if (session->secmode & SMB2_NEGOTIATE_SIGNING_REQUIRED)
			sign->flags |= SMB_SIGNING_CHECK;
	}

	/*
	 * If we just turned on signing, the current request
	 * (an SMB2 session setup) will have come in without
	 * SMB2_FLAGS_SIGNED (and not signed) but the response
	 * is is supposed to be signed. [MS-SMB2] 3.3.5.5
	 */
	if (sign->flags & SMB_SIGNING_ENABLED)
		sr->smb2_hdr_flags |= SMB2_FLAGS_SIGNED;
}

/*
 * smb2_sign_calc
 *
 * Calculates MAC signature for the given buffer and returns
 * it in the mac_sign parameter.
 *
 * The sequence number is in the last 16 bytes of the SMB2 header.
 * The signature algorighm is to compute HMAC SHA256 over the
 * entire command, with the signature field set to zeros.
 *
 * Return 0 if  success else -1
 */
int
smb2_sign_calc(struct mbuf_chain *mbc,
    struct smb_sign *sign,
    uint8_t *digest)
{
	sha2_hc_ctx_t hctx;
	uint8_t tmp_hdr[SMB2_HDR_SIZE];
	struct mbuf *mbuf;
	int offset = mbc->chain_offset;
	int resid = mbc->max_bytes - offset;
	int tlen;

	if (sign->mackey == NULL)
		return (-1);

	bzero(&hctx, sizeof (hctx));
	if (smb2_hmac_sha256_init(&hctx, sign->mackey, sign->mackey_len))
		return (-1);

	/*
	 * Work with a copy of the SMB2 header so we can
	 * clear the signature field without modifying
	 * the original message.
	 */
	tlen = SMB2_HDR_SIZE;
	if (smb_mbc_peek(mbc, offset, "#c", tlen, tmp_hdr) != 0)
		return (-1);
	bzero(tmp_hdr + SMB2_SIG_OFFS, SMB2_SIG_SIZE);
	smb2_hmac_sha256_update(&hctx, tmp_hdr, tlen);
	offset += tlen;
	resid -= tlen;

	/*
	 * Digest the rest of the SMB packet, starting at the data
	 * just after the SMB header.
	 *
	 * Advance to the src mbuf where we start digesting.
	 */
	mbuf = mbc->chain;
	while (mbuf != NULL && (offset >= mbuf->m_len)) {
		offset -= mbuf->m_len;
		mbuf = mbuf->m_next;
	}

	if (mbuf == NULL)
		return (-1);

	/*
	 * Digest the remainder of this mbuf, limited to the
	 * residual count, and starting at the current offset.
	 * (typically SMB2_HDR_SIZE)
	 */
	tlen = mbuf->m_len - offset;
	if (tlen > resid)
		tlen = resid;
	smb2_hmac_sha256_update(&hctx, (uint8_t *)mbuf->m_data + offset, tlen);
	resid -= tlen;

	/*
	 * Digest any more mbufs in the chain.
	 */
	while (resid > 0) {
		mbuf = mbuf->m_next;
		if (mbuf == NULL)
			return (-1);
		tlen = mbuf->m_len;
		if (tlen > resid)
			tlen = resid;
		smb2_hmac_sha256_update(&hctx, (uint8_t *)mbuf->m_data, tlen);
		resid -= tlen;
	}

	smb2_hmac_sha256_final(&hctx, digest);
	return (0);
}


/*
 * smb2_sign_check_request
 *
 * Calculates MAC signature for the request mbuf chain
 * using the next expected sequence number and compares
 * it to the given signature.
 *
 * Note it does not check the signature for secondary transactions
 * as their sequence number is the same as the original request.
 *
 * Return 0 if the signature verifies, otherwise, returns -1;
 *
 */
int
smb2_sign_check_request(smb_request_t *sr)
{
	uint8_t req_sig[SMB2_SIG_SIZE];
	uint8_t digest[SHA256_DIGEST_LENGTH];
	struct mbuf_chain *mbc = &sr->smb_data;
	struct smb_sign *sign = &sr->session->signing;
	int sig_off;

	/*
	 * Don't check commands with a zero session ID.
	 * [MS-SMB2] 3.3.4.1.1
	 */
	if (sr->smb_uid == 0)
		return (0);

	/* Get the request signature. */
	sig_off = sr->smb2_cmd_hdr + SMB2_SIG_OFFS;
	if (smb_mbc_peek(mbc, sig_off, "#c", SMB2_SIG_SIZE, req_sig) != 0)
		return (-1);

	/* Compute what we think it should be. */
	if (smb2_sign_calc(mbc, sign, digest) != 0)
		return (-1);

	if (memcmp(digest, req_sig, SMB2_SIG_SIZE) != 0) {
		cmn_err(CE_NOTE, "smb2_sign_check_request: bad signature");
		return (-1);
	}

	return (0);
}

/*
 * smb2_sign_reply
 *
 * Calculates MAC signature for the given mbuf chain,
 * and write it to the signature field in the mbuf.
 *
 */
void
smb2_sign_reply(smb_request_t *sr)
{
	uint8_t digest[SHA256_DIGEST_LENGTH];
	struct mbuf_chain tmp_mbc;
	struct smb_sign *sign = &sr->session->signing;
	int hdr_off, msg_len;

	msg_len = sr->reply.chain_offset - sr->smb2_reply_hdr;
	(void) MBC_SHADOW_CHAIN(&tmp_mbc, &sr->reply,
	    sr->smb2_reply_hdr, msg_len);

	/*
	 * Calculate MAC signature
	 */
	if (smb2_sign_calc(&tmp_mbc, sign, digest) != 0)
		return;

	/*
	 * Poke the signature into the response.
	 */
	hdr_off = sr->smb2_reply_hdr + SMB2_SIG_OFFS;
	(void) smb_mbc_poke(&sr->reply, hdr_off, "#c",
	    SMB2_SIG_SIZE, digest);
}

/*
 * There is no convenient interface for calling common code for i.e.
 * CKM_SHA256_HMAC in both user and kernel space, so these wrappers
 * around the low-level SHA2 functions do just what SMB2 needs for
 * message integrity signing.  Later, would be nice to come up
 * with a way to use KCF or PKCS11 (kernel, user) respectively.
 *
 * See also: uts/common/crypto/api/kcf_mac.c
 * lib/pkcs11/pkcs11_softtoken/common/softMAC.[ch]
 */


/*
 * Like pkcs11:mac_init_ctx()
 */
static int
smb2_hmac_sha256_init(sha2_hc_ctx_t *hc, uint8_t *key, uint_t key_len)
{
	uint64_t sha_ipad[SHA256_HMAC_INTS_PER_BLOCK];
	uint64_t sha_opad[SHA256_HMAC_INTS_PER_BLOCK];
	sha2_mech_type_t mech = SHA256_HMAC_MECH_INFO_TYPE;
	int i;

	bzero(sha_ipad, sizeof (sha_ipad));
	bzero(sha_opad, sizeof (sha_opad));

	if (key_len > SHA256_HMAC_BLOCK_SIZE) {
		/*
		 * Digest the key when it is longer than 64 bytes.
		 * (Not needed for SMB2 signing.)
		 */
		return (-1);
	}

	(void) memcpy(sha_ipad, key, key_len);
	(void) memcpy(sha_opad, key, key_len);

	/* XOR key with ipad (0x36) and opad (0x5c) */
	for (i = 0; i < SHA256_HMAC_INTS_PER_BLOCK; i ++) {
		sha_ipad[i] ^= 0x3636363636363636ULL;
		sha_opad[i] ^= 0x5c5c5c5c5c5c5c5cULL;
	}

	/* perform SHA2 on ipad */
	SHA2Init(mech, &hc->hc_icontext);
	SHA2Update(&hc->hc_icontext, (uint8_t *)sha_ipad, sizeof (sha_ipad));

	/* perform SHA2 on opad */
	SHA2Init(mech, &hc->hc_ocontext);
	SHA2Update(&hc->hc_ocontext, (uint8_t *)sha_opad, sizeof (sha_opad));

	return (0);
}

static void
smb2_hmac_sha256_update(sha2_hc_ctx_t *hc, uint8_t *data, uint_t data_len)
{

	/* SOFT_MAC_UPDATE(...) */
	SHA2Update(&hc->hc_icontext, data, data_len);
}

static void
smb2_hmac_sha256_final(sha2_hc_ctx_t *hc, uint8_t *digest)
{

	/* SOFT_MAC_FINAL_2(...) */
	SHA2Final(digest, &hc->hc_icontext);
	SHA2Update(&hc->hc_ocontext, digest, SHA256_DIGEST_LENGTH);
	SHA2Final(digest, &hc->hc_ocontext);
}
