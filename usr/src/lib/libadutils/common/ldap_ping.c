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

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <assert.h>
#include <stdlib.h>
#include <net/if.h>
#include <net/if.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <resolv.h>
#include <netdb.h>
#include <ctype.h>
#include <errno.h>
#include <ldap.h>
#include <lber.h>
#include <syslog.h>
#include "adutils_impl.h"
#include "addisc_impl.h"

#define	LDAP_PORT	389

#define	NETLOGON_ATTR_NAME			"NetLogon"
#define	NETLOGON_NT_VERSION_1			0x00000001
#define	NETLOGON_NT_VERSION_5			0x00000002
#define	NETLOGON_NT_VERSION_5EX			0x00000004
#define	NETLOGON_NT_VERSION_5EX_WITH_IP		0x00000008
#define	NETLOGON_NT_VERSION_WITH_CLOSEST_SITE	0x00000010
#define	NETLOGON_NT_VERSION_AVOID_NT4EMUL	0x01000000

typedef enum {
	OPCODE = 0,
	SBZ,
	FLAGS,
	DOMAIN_GUID,
	FOREST_NAME,
	DNS_DOMAIN_NAME,
	DNS_HOST_NAME,
	NET_DOMAIN_NAME,
	NET_COMP_NAME,
	USER_NAME,
	DC_SITE_NAME,
	CLIENT_SITE_NAME,
	SOCKADDR_SIZE,
	SOCKADDR,
	NEXT_CLOSEST_SITE_NAME,
	NTVER,
	LM_NT_TOKEN,
	LM_20_TOKEN
} field_5ex_t;

struct _berelement {
	char	*ber_buf;
	char	*ber_ptr;
	char	*ber_end;
};

extern int ldap_put_filter(BerElement *ber, char *);
static ad_disc_ds_t *find_ds_by_addr(ad_disc_ds_t *dclist,
    struct sockaddr_in6 *sin6);

static void
cldap_escape_le64(char *buf, uint64_t val, int bytes)
{
	char *p = buf;

	while (bytes != 0) {
		p += sprintf(p, "\\%.2x", (uint8_t)(val & 0xff));
		val >>= 8;
		bytes--;
	}
	*p = '\0';
}

/*
 * Construct CLDAPMessage PDU for NetLogon search request.
 *
 *  CLDAPMessage ::= SEQUENCE {
 *      messageID       MessageID,
 *      protocolOp      searchRequest   SearchRequest;
 *  }
 *
 *  SearchRequest ::=
 *      [APPLICATION 3] SEQUENCE {
 *          baseObject    LDAPDN,
 *          scope         ENUMERATED {
 *                             baseObject            (0),
 *                             singleLevel           (1),
 *                             wholeSubtree          (2)
 *                        },
 *          derefAliases  ENUMERATED {
 *                                     neverDerefAliases     (0),
 *                                     derefInSearching      (1),
 *                                     derefFindingBaseObj   (2),
 *                                     derefAlways           (3)
 *                                },
 *          sizeLimit     INTEGER (0 .. MaxInt),
 *          timeLimit     INTEGER (0 .. MaxInt),
 *          attrsOnly     BOOLEAN,
 *          filter        Filter,
 *          attributes    SEQUENCE OF AttributeType
 *  }
 */
BerElement *
cldap_build_request(const char *dname,
	const char *host, uint32_t ntver, uint16_t msgid)
{
	BerElement 	*ber;
	int		len = 0;
	char		*basedn = "";
	int scope = LDAP_SCOPE_BASE, deref = LDAP_DEREF_NEVER,
	    sizelimit = 0, timelimit = 0, attrsonly = 0;
	char		filter[512];
	char		ntver_esc[13];
	char		*p, *pend;

	/*
	 * Construct search filter in LDAP format.
	 */
	p = filter;
	pend = p + sizeof (filter);

	len = snprintf(p, pend - p, "(&(DnsDomain=%s)", dname);
	if (len >= (pend - p))
		goto fail;
	p += len;

	if (host != NULL) {
		len = snprintf(p, (pend - p), "(Host=%s)", host);
		if (len >= (pend - p))
			goto fail;
		p += len;
	}

	if (ntver != 0) {
		/*
		 * Format NtVer as little-endian with LDAPv3 escapes.
		 */
		cldap_escape_le64(ntver_esc, ntver, sizeof (ntver));
		len = snprintf(p, (pend - p), "(NtVer=%s)", ntver_esc);
		if (len >= (pend - p))
			goto fail;
		p += len;
	}

	len = snprintf(p, pend - p, ")");
	if (len >= (pend - p))
		goto fail;
	p += len;

	/*
	 * Encode CLDAPMessage and beginning of SearchRequest sequence.
	 */

	if ((ber = ber_alloc()) == NULL)
		goto fail;

	if (ber_printf(ber, "{it{seeiib", msgid,
	    LDAP_REQ_SEARCH, basedn, scope, deref,
	    sizelimit, timelimit, attrsonly) < 0)
		goto fail;

	/*
	 * Encode Filter sequence.
	 */
	if (ldap_put_filter(ber, filter) < 0)
		goto fail;
	/*
	 * Encode attribute and close Filter and SearchRequest sequences.
	 */
	if (ber_printf(ber, "{s}}}", NETLOGON_ATTR_NAME) < 0)
		goto fail;

	/*
	 * Success
	 */
	return (ber);

fail:
	if (ber != NULL)
		ber_free(ber, 1);
	return (NULL);
}

/*
 * Parse incoming search responses and attribute to correct hosts.
 *
 *  CLDAPMessage ::= SEQUENCE {
 *     messageID       MessageID,
 *                     searchResponse  SEQUENCE OF
 *                                         SearchResponse;
 *  }
 *
 *  SearchResponse ::=
 *    CHOICE {
 *         entry          [APPLICATION 4] SEQUENCE {
 *                             objectName     LDAPDN,
 *                             attributes     SEQUENCE OF SEQUENCE {
 *                                              AttributeType,
 *                                              SET OF
 *                                                AttributeValue
 *                                            }
 *                        },
 *         resultCode     [APPLICATION 5] LDAPResult
 *    }
 */

static int
decode_name(uchar_t *base, uchar_t *cp, char *str)
{
	uchar_t *tmp = NULL, *st = cp;
	uint8_t len;

	/*
	 * there should probably be some boundary checks on str && cp
	 * maybe pass in strlen && msglen ?
	 */
	while (*cp != 0) {
		if (*cp == 0xc0) {
			if (tmp == NULL)
				tmp = cp + 2;
			cp = base + *(cp + 1);
		}
		for (len = *cp++; len > 0; len--)
			*str++ = *cp++;
		*str++ = '.';
	}
	if (cp != st)
		*(str-1) = '\0';
	else
		*str = '\0';

	return ((tmp == NULL ? cp + 1 : tmp) - st);
}

int
cldap_parse(ad_disc_t ctx, ad_disc_ds_t *dc, BerElement *ber)
{
	uchar_t *base = NULL, *cp = NULL;
	char val[512]; /* how big should val be? */
	int l, msgid, rc = 0;
	uint16_t opcode;
	field_5ex_t f = OPCODE;

	/*
	 * Later, compare msgid's/some validation?
	 */

	if (ber_scanf(ber, "{i{x{{x[la", &msgid, &l, &cp) == LBER_ERROR) {
		rc = 1;
		goto out;
	}

	for (base = cp; ((cp - base) < l) && (f <= LM_20_TOKEN); f++) {
		val[0] = '\0';
		switch (f) {
		case OPCODE:
			/* opcode = *(uint16_t *)cp; */
			/* cp +=2; */
			opcode = *cp++;
			opcode |= (*cp++ << 8);
			break;
		case SBZ:
			cp += 2;
			break;
		case FLAGS:
			/* dci->Flags = *(uint32_t *)cp; */
			/* cp +=4; */
			dc->flags = *cp++;
			dc->flags |= (*cp++ << 8);
			dc->flags |= (*cp++ << 16);
			dc->flags |= (*cp++ << 26);
			break;
		case DOMAIN_GUID:
			if (ctx != NULL)
				auto_set_DomainGUID(ctx, cp);
			cp += 16;
			break;
		case FOREST_NAME:
			cp += decode_name(base, cp, val);
			if (ctx != NULL)
				auto_set_ForestName(ctx, val);
			break;
		case DNS_DOMAIN_NAME:
			/*
			 * We always have this already.
			 * (Could validate it here.)
			 */
			cp += decode_name(base, cp, val);
			break;
		case DNS_HOST_NAME:
			cp += decode_name(base, cp, val);
			if (0 != strcasecmp(val, dc->host)) {
				logger(LOG_ERR, "DC name %s != %s?",
				    val, dc->host);
			}
			break;
		case NET_DOMAIN_NAME:
			/*
			 * This is the "Flat" domain name.
			 * (i.e. the NetBIOS name)
			 * ignore for now.
			 */
			cp += decode_name(base, cp, val);
			break;
		case NET_COMP_NAME:
			/* not needed */
			cp += decode_name(base, cp, val);
			break;
		case USER_NAME:
			/* not needed */
			cp += decode_name(base, cp, val);
			break;
		case DC_SITE_NAME:
			cp += decode_name(base, cp, val);
			(void) strlcpy(dc->site, val, sizeof (dc->site));
			break;
		case CLIENT_SITE_NAME:
			cp += decode_name(base, cp, val);
			if (ctx != NULL)
				auto_set_SiteName(ctx, val);
			break;
		/*
		 * These are all possible, but we don't really care about them.
		 * Sockaddr_size && sockaddr might be useful at some point
		 */
		case SOCKADDR_SIZE:
		case SOCKADDR:
		case NEXT_CLOSEST_SITE_NAME:
		case NTVER:
		case LM_NT_TOKEN:
		case LM_20_TOKEN:
			break;
		default:
			rc = 3;
			goto out;
		}
	}

out:
	if (base)
		free(base);
	else if (cp)
		free(cp);
	return (rc);
}


/*
 * Filter out unresponsive servers, and save the domain info
 * returned by the "LDAP ping" in the returned object.
 * If ctx != NULL, this is a query for a DC, in which case we
 * also save the Domain GUID, Site name, and Forest name as
 * "auto" (discovered) values in the ctx.
 *
 * Only return the "winner".  (We only want one DC/GC)
 */
ad_disc_ds_t *
ldap_ping(ad_disc_t ctx, ad_disc_ds_t *dclist, char *dname, int reqflags)
{
	struct sockaddr_in6 addr6;
	socklen_t addrlen;
	struct pollfd pingchk;
	ad_disc_ds_t *send_ds;
	ad_disc_ds_t *recv_ds = NULL;
	ad_disc_ds_t *ret_ds = NULL;
	BerElement *req = NULL;
	BerElement *res = NULL;
	struct _berelement *be, *rbe;
	size_t be_len, rbe_len;
	int fd = -1;
	int tries = 3;
	int waitsec;
	int r;
	uint16_t msgid;

	if (dclist == NULL)
		return (dclist);

	/* One plus a null entry. */
	ret_ds = calloc(2, sizeof (ad_disc_ds_t));
	if (ret_ds == NULL)
		goto fail;

	if ((fd = socket(PF_INET6, SOCK_DGRAM, 0)) < 0)
		goto fail;

	(void) memset(&addr6, 0, sizeof (addr6));
	addr6.sin6_family = AF_INET6;
	addr6.sin6_addr = in6addr_any;
	if (bind(fd, (struct sockaddr *)&addr6, sizeof (addr6)) < 0)
		goto fail;

	/*
	 * semi-unique msgid...
	 */
	msgid = gethrtime() & 0xffff;

	/*
	 * Is ntver right? It certainly works on w2k8... If others are needed,
	 * that might require changes to cldap_parse
	 */
	req = cldap_build_request(dname, NULL,
	    NETLOGON_NT_VERSION_5EX, msgid);
	if (req == NULL)
		goto fail;
	be = (struct _berelement *)req;
	be_len = be->ber_end - be->ber_buf;

	if ((res = ber_alloc()) == NULL)
		goto fail;
	rbe = (struct _berelement *)res;
	rbe_len = rbe->ber_end - rbe->ber_buf;

	pingchk.fd = fd;
	pingchk.events = POLLIN;
	pingchk.revents = 0;

try_again:
	send_ds = dclist;
	waitsec = 5;
	while (recv_ds == NULL && waitsec > 0) {

		/*
		 * If there is another candidate, send to it.
		 */
		if (send_ds->host[0] != '\0') {
			/*
			 * Build "to" address.
			 */
			(void) memset(&addr6, 0, sizeof (addr6));
			if (send_ds->addr.ss_family == AF_INET6) {
				(void) memcpy(&addr6, &send_ds->addr,
				    sizeof (addr6));
			} else if (send_ds->addr.ss_family == AF_INET) {
				struct sockaddr_in *sin =
				    (void *)&send_ds->addr;
				addr6.sin6_family = AF_INET6;
				IN6_INADDR_TO_V4MAPPED(
				    &sin->sin_addr, &addr6.sin6_addr);
			} else {
				logger(LOG_ERR, "No addr for %s",
				    send_ds->host);
				send_ds++;
				continue;
			}
			addr6.sin6_port = htons(LDAP_PORT);

			/*
			 * "Ping" this candidate.
			 */
			(void) sendto(fd, be->ber_buf, be_len, 0,
			    (struct sockaddr *)&addr6, sizeof (addr6));
			send_ds++;

			/*
			 * Wait 1/10 sec. before the next send.
			 */
			r = poll(&pingchk, 1, 100);
		} else {
			/*
			 * No more candidates to "ping", so
			 * just wait a sec for responses.
			 */
			r = poll(&pingchk, 1, 1000);
			if (r == 0)
				--waitsec;
		}

		if (r > 0) {
			/*
			 * Got a response.
			 */
			(void) memset(&addr6, 0, addrlen = sizeof (addr6));
			r = recvfrom(fd, rbe->ber_buf, rbe_len, 0,
			    (struct sockaddr *)&addr6, &addrlen);

			recv_ds = find_ds_by_addr(dclist, &addr6);
			if (recv_ds == NULL) {
				char abuf[64];
				const char *paddr;
				paddr = inet_ntop(AF_INET6,
				    &addr6.sin6_addr,
				    abuf, sizeof (abuf));
				logger(LOG_ERR, "Unsolicited response from %s",
				    paddr);
				continue;
			}
			(void) cldap_parse(ctx, recv_ds, res);
			if ((recv_ds->flags & reqflags) != reqflags) {
				logger(LOG_ERR, "Skip %s"
				    "due to flags 0x%X",
				    recv_ds->host,
				    recv_ds->flags);
				recv_ds = NULL;
			}
		}
	}

	if (recv_ds == NULL) {
		if (--tries <= 0)
			goto fail;
		goto try_again;
	}

	(void) memcpy(ret_ds, recv_ds, sizeof (*ret_ds));

	ber_free(res, 1);
	ber_free(req, 1);
	(void) close(fd);
	free(dclist);
	return (ret_ds);

fail:
	ber_free(res, 1);
	ber_free(req, 1);
	(void) close(fd);
	free(dclist);
	free(ret_ds);
	return (NULL);
}

static ad_disc_ds_t *
find_ds_by_addr(ad_disc_ds_t *dclist, struct sockaddr_in6 *sin6from)
{
	ad_disc_ds_t *ds;

	/*
	 * Find the DS this response came from.
	 * (don't accept unsolicited responses)
	 *
	 * Note: on a GC query, the ds->addr port numbers are
	 * the GC port, and our from addr has the LDAP port.
	 * Just compare the IP addresses.
	 */
	for (ds = dclist; ds->host[0] != '\0'; ds++) {
		if (ds->addr.ss_family == AF_INET6) {
			struct sockaddr_in6 *sin6p = (void *)&ds->addr;

			if (!memcmp(&sin6from->sin6_addr, &sin6p->sin6_addr,
			    sizeof (struct in6_addr)))
				break;
		}
		if (ds->addr.ss_family == AF_INET) {
			struct in6_addr in6;
			struct sockaddr_in *sin4p = (void *)&ds->addr;

			IN6_INADDR_TO_V4MAPPED(&sin4p->sin_addr, &in6);
			if (!memcmp(&sin6from->sin6_addr, &in6,
			    sizeof (struct in6_addr)))
				break;
		}
	}
	if (ds->host[0] == '\0')
		ds = NULL;

	return (ds);
}
