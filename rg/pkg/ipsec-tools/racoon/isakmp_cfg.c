/* $Id: isakmp_cfg.c,v 1.1.6.2 2012/02/10 06:32:34 simba Exp $ */

/*
 * Copyright (C) 2004-2006 Emmanuel Dreyfus
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "config.h"

#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/queue.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#if TIME_WITH_SYS_TIME
# include <sys/time.h>
# include <time.h>
#else
# if HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif
#include <netdb.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <ctype.h>

#ifdef HAVE_LIBRADIUS
#include <sys/utsname.h>
#include <radlib.h>
#endif

#include "var.h"
#include "misc.h"
#include "vmbuf.h"
#include "plog.h"
#include "sockmisc.h"
#include "schedule.h"
#include "debug.h"

#include "isakmp_var.h"
#include "isakmp.h"
#include "handler.h"
#include "evt.h"
#include "throttle.h"
#include "remoteconf.h"
#include "crypto_openssl.h"
#include "isakmp_inf.h"
#include "isakmp_xauth.h"
#include "isakmp_unity.h"
#include "isakmp_cfg.h"
#include "strnames.h"
#include "admin.h"
#include "privsep.h"

struct isakmp_cfg_config isakmp_cfg_config = {
	0x00000000, 	/* network4 */
	0x00000000,	/* netmask4 */
	0x00000000,	/* dns4 */
	0x00000000,	/* nbns4 */
	NULL,		/* pool */
	ISAKMP_CFG_AUTH_SYSTEM,		/* authsource */
	ISAKMP_CFG_CONF_LOCAL,		/* confsource */
	ISAKMP_CFG_ACCT_NONE,		/* accounting */
	ISAKMP_CFG_MAX_CNX,		/* pool_size */
	THROTTLE_PENALTY,		/* auth_throttle */
	ISAKMP_CFG_MOTD,		/* motd */
	0,				/* pfs_group */
	0,				/* save_passwd */
};

static vchar_t *buffer_cat(vchar_t *s, vchar_t *append);
static vchar_t *isakmp_cfg_net(struct ph1handle *, struct isakmp_data *);
#if 0
static vchar_t *isakmp_cfg_void(struct ph1handle *, struct isakmp_data *);
#endif
static vchar_t *isakmp_cfg_addr4(struct ph1handle *, 
				 struct isakmp_data *, in_addr_t *);
static void isakmp_cfg_getaddr4(struct isakmp_data *, struct in_addr *);

#define ISAKMP_CFG_LOGIN	1
#define ISAKMP_CFG_LOGOUT	2
static int isakmp_cfg_accounting(struct ph1handle *, int);
#ifdef HAVE_LIBRADIUS
static int isakmp_cfg_accounting_radius(struct ph1handle *, int);
#endif

/* 
 * Handle an ISAKMP config mode packet
 * We expect HDR, HASH, ATTR
 */
void
isakmp_cfg_r(iph1, msg)
	struct ph1handle *iph1;
	vchar_t *msg;
{
	struct isakmp *packet;
	struct isakmp_gen *ph;
	int tlen;
	char *npp;
	int np;
	vchar_t *dmsg;
	struct isakmp_ivm *ivm;

	/* Check that the packet is long enough to have a header */
	if (msg->l < sizeof(*packet)) {
	     plog(LLV_ERROR, LOCATION, NULL, "Unexpected short packet\n");
	     return;
	}

	packet = (struct isakmp *)msg->v;

	/* Is it encrypted? It should be encrypted */
	if ((packet->flags & ISAKMP_FLAG_E) == 0) {
		plog(LLV_ERROR, LOCATION, NULL, 
		    "User credentials sent in cleartext!\n");
		return;
	}

	/* 
	 * Decrypt the packet. If this is the beginning of a new
	 * exchange, reinitialize the IV
	 */
	if (iph1->mode_cfg->ivm == NULL)
		iph1->mode_cfg->ivm = 
		    isakmp_cfg_newiv(iph1, packet->msgid);
	ivm = iph1->mode_cfg->ivm;

	dmsg = oakley_do_decrypt(iph1, msg, ivm->iv, ivm->ive);
	if (dmsg == NULL) {
		plog(LLV_ERROR, LOCATION, NULL, 
		    "failed to decrypt message\n");
		return;
	}

	plog(LLV_DEBUG, LOCATION, NULL, "MODE_CFG packet\n");
	plogdump(LLV_DEBUG, dmsg->v, dmsg->l);

	/* Now work with the decrypted packet */
	packet = (struct isakmp *)dmsg->v;
	tlen = dmsg->l - sizeof(*packet);
	ph = (struct isakmp_gen *)(packet + 1);

	np = packet->np;
	while ((tlen > 0) && (np != ISAKMP_NPTYPE_NONE)) {
		/* Check that the payload header fits in the packet */
		if (tlen < sizeof(*ph)) {
			 plog(LLV_WARNING, LOCATION, NULL, 
			      "Short payload header\n");
			 goto out;
		}

		/* Check that the payload fits in the packet */
		if (tlen < ntohs(ph->len)) {
			plog(LLV_WARNING, LOCATION, NULL, 
			      "Short payload\n");
			goto out;
		}
		
		plog(LLV_DEBUG, LOCATION, NULL, "Seen payload %d\n", np);
		plogdump(LLV_DEBUG, ph, ntohs(ph->len));

		switch(np) {
		case ISAKMP_NPTYPE_HASH: {
			vchar_t *check;
			vchar_t *payload;
			size_t plen;
			struct isakmp_gen *nph;

			plen = ntohs(ph->len);
			nph = (struct isakmp_gen *)((char *)ph + plen);
			plen = ntohs(nph->len);

			if ((payload = vmalloc(plen)) == NULL) {
				plog(LLV_ERROR, LOCATION, NULL, 
				    "Cannot allocate memory\n");
				goto out;
			}
			memcpy(payload->v, nph, plen);

			if ((check = oakley_compute_hash1(iph1, 
			    packet->msgid, payload)) == NULL) {
				plog(LLV_ERROR, LOCATION, NULL, 
				    "Cannot compute hash\n");
				vfree(payload);
				goto out;
			}

			if (memcmp(ph + 1, check->v, check->l) != 0) {
				plog(LLV_ERROR, LOCATION, NULL, 
				    "Hash verification failed\n");
				vfree(payload);
				vfree(check);
				goto out;
			}
			vfree(payload);
			vfree(check);
			break;
		}
		case ISAKMP_NPTYPE_ATTR: {
			struct isakmp_pl_attr *attrpl;

			attrpl = (struct isakmp_pl_attr *)ph;
			isakmp_cfg_attr_r(iph1, packet->msgid, attrpl);

			break;
		}
		default:
			 plog(LLV_WARNING, LOCATION, NULL, 
			      "Unexpected next payload %d\n", np);
			 /* Skip to the next payload */
			 break;
		}

		/* Move to the next payload */
		np = ph->np;
		tlen -= ntohs(ph->len);
		npp = (char *)ph;
		ph = (struct isakmp_gen *)(npp + ntohs(ph->len));
	}

out:
	vfree(dmsg);
}

int
isakmp_cfg_attr_r(iph1, msgid, attrpl) 
	struct ph1handle *iph1;
	u_int32_t msgid;
	struct isakmp_pl_attr *attrpl;
{
	int type = attrpl->type;

	switch (type) {
	case ISAKMP_CFG_ACK:
		/* ignore, but this is the time to reinit the IV */
		oakley_delivm(iph1->mode_cfg->ivm);
		iph1->mode_cfg->ivm = NULL;
		return 0;
		break;

	case ISAKMP_CFG_REPLY:
		return isakmp_cfg_reply(iph1, attrpl);
		break;

	case ISAKMP_CFG_REQUEST:
		iph1->msgid = msgid;
		return isakmp_cfg_request(iph1, attrpl);
		break;

	case ISAKMP_CFG_SET:
		iph1->msgid = msgid;
		return isakmp_cfg_set(iph1, attrpl);
		break;

	default:
		plog(LLV_WARNING, LOCATION, NULL,
		     "Unepected configuration exchange type %d\n", type);
		return -1;
		break;
	}

	return 0;
}

int
isakmp_cfg_reply(iph1, attrpl)
	struct ph1handle *iph1;
	struct isakmp_pl_attr *attrpl;
{
	struct isakmp_data *attr;
	int tlen;
	size_t alen;
	char *npp;
	int type;
	struct sockaddr_in *sin;

	tlen = ntohs(attrpl->h.len);
	attr = (struct isakmp_data *)(attrpl + 1);
	tlen -= sizeof(*attrpl);
	
	while (tlen > 0) {
		type = ntohs(attr->type);

		/* Handle short attributes */
		if ((type & ISAKMP_GEN_MASK) == ISAKMP_GEN_TV) {
			type &= ~ISAKMP_GEN_MASK;

			plog(LLV_DEBUG, LOCATION, NULL,
			     "Short attribute %d = %d\n", 
			     type, ntohs(attr->lorv));

			switch (type) {
			case XAUTH_TYPE:
				xauth_attr_reply(iph1, attr, ntohs(attrpl->id));
				break;

			default:
				plog(LLV_WARNING, LOCATION, NULL,
				     "Ignored short attribute %d\n", type);
				break;
			}

			tlen -= sizeof(*attr);
			attr++;
			continue;
		}

		type = ntohs(attr->type);
		alen = ntohs(attr->lorv);

		/* Check that the attribute fit in the packet */
		if (tlen < alen) {
			plog(LLV_ERROR, LOCATION, NULL,
			     "Short attribute %d\n", type);
			return -1;
		}

		plog(LLV_DEBUG, LOCATION, NULL,
		     "Attribute %d, len %zu\n", type, alen);

		switch(type) {
		case XAUTH_TYPE:
		case XAUTH_USER_NAME:
		case XAUTH_USER_PASSWORD:
		case XAUTH_PASSCODE:
		case XAUTH_MESSAGE:
		case XAUTH_CHALLENGE:
		case XAUTH_DOMAIN:
		case XAUTH_STATUS:
		case XAUTH_NEXT_PIN:
		case XAUTH_ANSWER:
			xauth_attr_reply(iph1, attr, ntohs(attrpl->id));
			break;
		case INTERNAL_IP4_ADDRESS:
			isakmp_cfg_getaddr4(attr, &iph1->mode_cfg->addr4);
			iph1->mode_cfg->flags |= ISAKMP_CFG_GOT_ADDR4;
			break;
		case INTERNAL_IP4_NETMASK:
			isakmp_cfg_getaddr4(attr, &iph1->mode_cfg->mask4);
			iph1->mode_cfg->flags |= ISAKMP_CFG_GOT_MASK4;
			break;
		case INTERNAL_IP4_DNS:
			isakmp_cfg_getaddr4(attr, &iph1->mode_cfg->dns4);
			iph1->mode_cfg->flags |= ISAKMP_CFG_GOT_DNS4;
			break;
		case INTERNAL_IP4_NBNS:
			isakmp_cfg_getaddr4(attr, &iph1->mode_cfg->wins4);
			iph1->mode_cfg->flags |= ISAKMP_CFG_GOT_WINS4;
			break;
		case INTERNAL_IP4_SUBNET:
		case INTERNAL_ADDRESS_EXPIRY:
		case UNITY_BANNER:
		case UNITY_SAVE_PASSWD:
		case UNITY_DEF_DOMAIN:
		case UNITY_SPLITDNS_NAME:
		case UNITY_SPLIT_INCLUDE:
		case UNITY_NATT_PORT:
		case UNITY_PFS:
		case UNITY_FW_TYPE:
		case UNITY_BACKUP_SERVERS:
		case UNITY_DDNS_HOSTNAME:
		default:
			plog(LLV_WARNING, LOCATION, NULL,
			     "Ignored attribute %d\n", type);
			break;
		}

		npp = (char *)attr;
		attr = (struct isakmp_data *)(npp + sizeof(*attr) + alen);
		tlen -= (sizeof(*attr) + alen);
	}

	/* 
	 * Call the SA up script hook now that we have the configuration
	 * It is done at the end of phase 1 if ISAKMP mode config is not
	 * requested.
	 */
	if ((iph1->status == PHASE1ST_ESTABLISHED) && 
	    iph1->rmconf->mode_cfg)
		script_hook(iph1, SCRIPT_PHASE1_UP);

#ifdef ENABLE_ADMINPORT
	{
		vchar_t *buf;

		alen = ntohs(attrpl->h.len) - sizeof(*attrpl);
		if ((buf = vmalloc(alen)) == NULL) {
			plog(LLV_WARNING, LOCATION, NULL, 
			    "Cannot allocate memory: %s\n", strerror(errno));
		} else {
			memcpy(buf->v, attrpl + 1, buf->l);
			EVT_PUSH(iph1->local, iph1->remote, 
			    EVTT_ISAKMP_CFG_DONE, buf);
			vfree(buf);
		}
	}
#endif

	return 0;
}

int
isakmp_cfg_request(iph1, attrpl)
	struct ph1handle *iph1;
	struct isakmp_pl_attr *attrpl;
{
	struct isakmp_data *attr;
	int tlen;
	size_t alen;
	char *npp;
	vchar_t *payload;
	struct isakmp_pl_attr *reply;
	vchar_t *reply_attr;
	int type;
	int error = -1;

	if ((payload = vmalloc(sizeof(*reply))) == NULL) {
		plog(LLV_ERROR, LOCATION, NULL, "Cannot allocate memory\n");
		return -1;
	}
	memset(payload->v, 0, sizeof(*reply));

	tlen = ntohs(attrpl->h.len);
	attr = (struct isakmp_data *)(attrpl + 1);
	tlen -= sizeof(*attrpl);
	
	while (tlen > 0) {
		reply_attr = NULL;
		type = ntohs(attr->type);

		/* Handle short attributes */
		if ((type & ISAKMP_GEN_MASK) == ISAKMP_GEN_TV) {
			type &= ~ISAKMP_GEN_MASK;

			plog(LLV_DEBUG, LOCATION, NULL,
			     "Short attribute %d = %d\n", 
			     type, ntohs(attr->lorv));

			switch (type) {
			case XAUTH_TYPE:
				reply_attr = isakmp_xauth_req(iph1, attr);
				break;
			default:
				plog(LLV_WARNING, LOCATION, NULL,
				     "Ignored short attribute %d\n", type);
				break;
			}

			tlen -= sizeof(*attr);
			attr++;

			if (reply_attr != NULL) {
				payload = buffer_cat(payload, reply_attr);
				vfree(reply_attr);
			}

			continue;
		}

		type = ntohs(attr->type);
		alen = ntohs(attr->lorv);

		/* Check that the attribute fit in the packet */
		if (tlen < alen) {
			plog(LLV_ERROR, LOCATION, NULL,
			     "Short attribute %d\n", type);
			goto end;
		}

		plog(LLV_DEBUG, LOCATION, NULL,
		     "Attribute %d, len %zu\n", type, alen);

		switch(type) {
		case INTERNAL_IP4_ADDRESS:
		case INTERNAL_IP4_NETMASK:
		case INTERNAL_IP4_DNS:
		case INTERNAL_IP4_NBNS:
		case INTERNAL_IP4_SUBNET:
			reply_attr = isakmp_cfg_net(iph1, attr);
			break;

		case XAUTH_TYPE:
		case XAUTH_USER_NAME:
		case XAUTH_USER_PASSWORD:
		case XAUTH_PASSCODE:
		case XAUTH_MESSAGE:
		case XAUTH_CHALLENGE:
		case XAUTH_DOMAIN:
		case XAUTH_STATUS:
		case XAUTH_NEXT_PIN:
		case XAUTH_ANSWER:
			reply_attr = isakmp_xauth_req(iph1, attr);
			break;

		case APPLICATION_VERSION:
			reply_attr = isakmp_cfg_string(iph1, 
			    attr, ISAKMP_CFG_RACOON_VERSION);
			break;

		case UNITY_BANNER:
		case UNITY_PFS:
		case UNITY_SAVE_PASSWD:
		case UNITY_DEF_DOMAIN:
		case UNITY_DDNS_HOSTNAME:
		case UNITY_FW_TYPE:
		case UNITY_SPLITDNS_NAME:
		case UNITY_SPLIT_INCLUDE:
		case UNITY_NATT_PORT:
		case UNITY_BACKUP_SERVERS:
			reply_attr = isakmp_unity_req(iph1, attr);
			break;

		case INTERNAL_ADDRESS_EXPIRY:
		default:
			plog(LLV_WARNING, LOCATION, NULL,
			     "Ignored attribute %d\n", type);
			break;
		}

		npp = (char *)attr;
		attr = (struct isakmp_data *)(npp + sizeof(*attr) + alen);
		tlen -= (sizeof(*attr) + alen);

		if (reply_attr != NULL) {
			payload = buffer_cat(payload, reply_attr);
			vfree(reply_attr);
		}

	}

	reply = (struct isakmp_pl_attr *)payload->v;
	reply->h.len = htons(payload->l);
	reply->type = ISAKMP_CFG_REPLY;
	reply->id = attrpl->id;

	error = isakmp_cfg_send(iph1, payload, 
	    ISAKMP_NPTYPE_ATTR, ISAKMP_FLAG_E, 0);

	/* Reinit the IV */
	oakley_delivm(iph1->mode_cfg->ivm);
	iph1->mode_cfg->ivm = NULL;
end:
	vfree(payload);

	return error;
}

int
isakmp_cfg_set(iph1, attrpl)
	struct ph1handle *iph1;
	struct isakmp_pl_attr *attrpl;
{
	struct isakmp_data *attr;
	int tlen;
	size_t alen;
	char *npp;
	vchar_t *payload;
	struct isakmp_pl_attr *reply;
	vchar_t *reply_attr;
	int type;
	int error = -1;

	if ((payload = vmalloc(sizeof(*reply))) == NULL) {
		plog(LLV_ERROR, LOCATION, NULL, "Cannot allocate memory\n");
		return -1;
	}
	memset(payload->v, 0, sizeof(*reply));

	tlen = ntohs(attrpl->h.len);
	attr = (struct isakmp_data *)(attrpl + 1);
	tlen -= sizeof(*attrpl);
	
	/* 
	 * We should send ack for the attributes we accepted 
	 */
	while (tlen > 0) {
		reply_attr = NULL;
		type = ntohs(attr->type);

		switch (type & ~ISAKMP_GEN_MASK) {
		case XAUTH_STATUS:
			reply_attr = isakmp_xauth_set(iph1, attr);
			break;
		default:
			plog(LLV_DEBUG, LOCATION, NULL,
			     "Unexpected SET attribute %d\n", 
				 type & ~ISAKMP_GEN_MASK);
			break;
		}

		if ((reply_attr = vmalloc(sizeof(*reply_attr))) != NULL) {
			payload = buffer_cat(payload, reply_attr);
			vfree(reply_attr);
		}

		/* 
		 * Move to next attribute. If we run out of the packet, 
		 * tlen becomes negative and we exit. 
		 */
		if ((type & ISAKMP_GEN_MASK) == ISAKMP_GEN_TV) {
			tlen -= sizeof(*attr);
			attr++;
		} else {
			alen = ntohs(attr->lorv);
			tlen -= (sizeof(*attr) + alen);
			npp = (char *)attr;
			attr = (struct isakmp_data *)
			    (npp + sizeof(*attr) + alen);
		}
	}

	reply = (struct isakmp_pl_attr *)payload->v;
	reply->h.len = htons(payload->l);
	reply->type = ISAKMP_CFG_ACK;
	reply->id = attrpl->id;

	error = isakmp_cfg_send(iph1, payload, 
	    ISAKMP_NPTYPE_ATTR, ISAKMP_FLAG_E, 0);

	if (iph1->mode_cfg->flags & ISAKMP_CFG_DELETE_PH1) {
		if (iph1->status == PHASE1ST_ESTABLISHED)
			isakmp_info_send_d1(iph1);
		remph1(iph1);
		delph1(iph1);
	}
end:
	vfree(payload);

	/* 
	 * If required, request ISAKMP mode config information
	 */
	if ((iph1->rmconf->mode_cfg) && (error == 0))
		error = isakmp_cfg_getconfig(iph1);

	return error;
}


static vchar_t *
buffer_cat(s, append)
	vchar_t *s;
	vchar_t *append;
{
	vchar_t *new;

	new = vmalloc(s->l + append->l);
	if (new == NULL) {
		plog(LLV_ERROR, LOCATION, NULL, 
		    "Cannot allocate memory\n");
		return s;
	}

	memcpy(new->v, s->v, s->l);
	memcpy(new->v + s->l, append->v, append->l);

	vfree(s);
	return new;
}

static vchar_t *
isakmp_cfg_net(iph1, attr)
	struct ph1handle *iph1;
	struct isakmp_data *attr;
{
	int type;
	in_addr_t addr4;

	type = ntohs(attr->type);

	/* 
	 * Don't give an address to a peer that did not succeed Xauth
	 */
	if (xauth_check(iph1) != 0) {
		plog(LLV_ERROR, LOCATION, NULL,
		    "Attempt to start phase config whereas Xauth failed\n");
		return NULL;
	}

	switch(type) {
	case INTERNAL_IP4_ADDRESS:
		switch(isakmp_cfg_config.confsource) {
#ifdef HAVE_LIBRADIUS
		case ISAKMP_CFG_CONF_RADIUS:
			if ((iph1->mode_cfg->flags & ISAKMP_CFG_ADDR4_RADIUS)
			    && (iph1->mode_cfg->addr4.s_addr != htonl(-2)))
			    /*
			     * -2 is 255.255.255.254, RADIUS uses that
			     * to instruct the NAS to use a local pool
			     */
			    break;
			plog(LLV_INFO, LOCATION, NULL, 
			    "No IP from RADIUS, using local pool\n");
			/* FALLTHROUGH */
#endif
		case ISAKMP_CFG_CONF_LOCAL:
			if (isakmp_cfg_getport(iph1) == -1) {
				plog(LLV_ERROR, LOCATION, NULL, 
				    "Port pool depleted\n");
				break;
			}

			iph1->mode_cfg->addr4.s_addr = 
			    htonl(ntohl(isakmp_cfg_config.network4) 
			    + iph1->mode_cfg->port);
			iph1->mode_cfg->flags |= ISAKMP_CFG_ADDR4_LOCAL;
			break;

		default:
			plog(LLV_ERROR, LOCATION, NULL, 
			    "Unexpected confsource\n");
		}
			
		if (isakmp_cfg_accounting(iph1, ISAKMP_CFG_LOGIN) != 0)
			plog(LLV_ERROR, LOCATION, NULL, "Accounting failed\n");

		return isakmp_cfg_addr4(iph1, 
		    attr, &iph1->mode_cfg->addr4.s_addr);
		break;

	case INTERNAL_IP4_NETMASK:
		switch(isakmp_cfg_config.confsource) {
#ifdef HAVE_LIBRADIUS
		case ISAKMP_CFG_CONF_RADIUS:
			if (iph1->mode_cfg->flags & ISAKMP_CFG_MASK4_RADIUS)
				break;
			plog(LLV_INFO, LOCATION, NULL, 
			    "No mask from RADIUS, using local pool\n");
			/* FALLTHROUGH */
#endif
		case ISAKMP_CFG_CONF_LOCAL:
			iph1->mode_cfg->mask4.s_addr 
			    = isakmp_cfg_config.netmask4;
			iph1->mode_cfg->flags |= ISAKMP_CFG_MASK4_LOCAL;
			break;

		default:
			plog(LLV_ERROR, LOCATION, NULL, 
			    "Unexpected confsource\n");
		}
		return isakmp_cfg_addr4(iph1, attr, 
		    &iph1->mode_cfg->mask4.s_addr);
		break;

	case INTERNAL_IP4_DNS:
		return isakmp_cfg_addr4(iph1, 
		    attr, &isakmp_cfg_config.dns4);
		break;

	case INTERNAL_IP4_NBNS:
		return isakmp_cfg_addr4(iph1, 
		    attr, &isakmp_cfg_config.nbns4);
		break;

	case INTERNAL_IP4_SUBNET:
		return isakmp_cfg_addr4(iph1, 
		    attr, &isakmp_cfg_config.network4);
		break;

	default:
		plog(LLV_ERROR, LOCATION, NULL, "Unexpected type %d\n", type);
		break;
	}

	return NULL;
}

#if 0
static vchar_t *
isakmp_cfg_void(iph1, attr)
	struct ph1handle *iph1;
	struct isakmp_data *attr;
{
	vchar_t *buffer;
	struct isakmp_data *new;

	if ((buffer = vmalloc(sizeof(*attr))) == NULL) {
		plog(LLV_ERROR, LOCATION, NULL, "Cannot allocate memory\n");
		return NULL;
	}

	new = (struct isakmp_data *)buffer->v;

	new->type = attr->type;
	new->lorv = htons(0);

	return buffer;
}
#endif

vchar_t *
isakmp_cfg_copy(iph1, attr)
	struct ph1handle *iph1;
	struct isakmp_data *attr;
{
	vchar_t *buffer;
	size_t len = 0;

	if ((ntohs(attr->type) & ISAKMP_GEN_MASK) == ISAKMP_GEN_TLV)
		len = ntohs(attr->lorv);

	if ((buffer = vmalloc(sizeof(*attr) + len)) == NULL) {
		plog(LLV_ERROR, LOCATION, NULL, "Cannot allocate memory\n");
		return NULL;
	}

	memcpy(buffer->v, attr, sizeof(*attr) + ntohs(attr->lorv));

	return buffer;
}

vchar_t *
isakmp_cfg_short(iph1, attr, value)
	struct ph1handle *iph1;
	struct isakmp_data *attr;
	int value;
{
	vchar_t *buffer;
	struct isakmp_data *new;
	int type;

	if ((buffer = vmalloc(sizeof(*attr))) == NULL) {
		plog(LLV_ERROR, LOCATION, NULL, "Cannot allocate memory\n");
		return NULL;
	}

	new = (struct isakmp_data *)buffer->v;
	type = ntohs(attr->type) & ~ISAKMP_GEN_MASK;

	new->type = htons(type | ISAKMP_GEN_TV);
	new->lorv = htons(value);

	return buffer;
}

vchar_t *
isakmp_cfg_string(iph1, attr, string)
	struct ph1handle *iph1;
	struct isakmp_data *attr;
	char *string;
{
	vchar_t *buffer;
	struct isakmp_data *new;
	size_t len;
	char *data;

	len = strlen(string);
	if ((buffer = vmalloc(sizeof(*attr) + len)) == NULL) {
		plog(LLV_ERROR, LOCATION, NULL, "Cannot allocate memory\n");
		return NULL;
	}

	new = (struct isakmp_data *)buffer->v;

	new->type = attr->type;
	new->lorv = htons(len);
	data = (char *)(new + 1);

	memcpy(data, string, len);
	
	return buffer;
}

static vchar_t *
isakmp_cfg_addr4(iph1, attr, addr)
	struct ph1handle *iph1;
	struct isakmp_data *attr;
	in_addr_t *addr;
{
	vchar_t *buffer;
	struct isakmp_data *new;
	size_t len;

	len = sizeof(*addr);
	if ((buffer = vmalloc(sizeof(*attr) + len)) == NULL) {
		plog(LLV_ERROR, LOCATION, NULL, "Cannot allocate memory\n");
		return NULL;
	}

	new = (struct isakmp_data *)buffer->v;

	new->type = attr->type;
	new->lorv = htons(len);
	memcpy(new + 1, addr, len);
	
	return buffer;
}

struct isakmp_ivm *
isakmp_cfg_newiv(iph1, msgid)
	struct ph1handle *iph1;
	u_int32_t msgid;
{
	struct isakmp_cfg_state *ics = iph1->mode_cfg;

	if (ics == NULL) {
		plog(LLV_ERROR, LOCATION, NULL,
		    "isakmp_cfg_newiv called without mode config state\n");
		return NULL;
	}

	if (ics->ivm != NULL)
		oakley_delivm(ics->ivm);

	ics->ivm = oakley_newiv2(iph1, msgid);

	return ics->ivm;
}

/* Derived from isakmp_info_send_common */
int
isakmp_cfg_send(iph1, payload, np, flags, new_exchange)
	struct ph1handle *iph1;
	vchar_t *payload;
	u_int32_t np;
	int flags;
	int new_exchange;
{
	struct ph2handle *iph2 = NULL;
	vchar_t *hash = NULL;
	struct isakmp *isakmp;
	struct isakmp_gen *gen;
	char *p;
	int tlen;
	int error = -1;
	struct isakmp_cfg_state *ics = iph1->mode_cfg;

	/* Check if phase 1 is established */
	if ((iph1->status != PHASE1ST_ESTABLISHED) || 
	    (iph1->local == NULL) ||
	    (iph1->remote == NULL)) {
		plog(LLV_ERROR, LOCATION, NULL, 
		    "ISAKMP mode config exchange with immature phase 1\n");
		goto end;
	}

	/* add new entry to isakmp status table */
	iph2 = newph2();
	if (iph2 == NULL)
		goto end;

	iph2->dst = dupsaddr(iph1->remote);
	iph2->src = dupsaddr(iph1->local);
	switch (iph1->remote->sa_family) {
	case AF_INET:
#ifndef ENABLE_NATT
		((struct sockaddr_in *)iph2->dst)->sin_port = 0;
		((struct sockaddr_in *)iph2->src)->sin_port = 0;
#endif
		break;
#ifdef INET6
	case AF_INET6:
		((struct sockaddr_in6 *)iph2->dst)->sin6_port = 0;
		((struct sockaddr_in6 *)iph2->src)->sin6_port = 0;
		break;
#endif
	default:
		plog(LLV_ERROR, LOCATION, NULL,
			"invalid family: %d\n", iph1->remote->sa_family);
		delph2(iph2);
		goto end;
	}
	iph2->ph1 = iph1;
	iph2->side = INITIATOR;
	iph2->status = PHASE2ST_START;

	if (new_exchange)
		iph2->msgid = isakmp_newmsgid2(iph1);
	else
		iph2->msgid = iph1->msgid;

	/* get IV and HASH(1) if skeyid_a was generated. */
	if (iph1->skeyid_a != NULL) {
		if (new_exchange) {
			if (isakmp_cfg_newiv(iph1, iph2->msgid) == NULL) {
				delph2(iph2);
				goto end;
			}
		}

		/* generate HASH(1) */
		hash = oakley_compute_hash1(iph2->ph1, iph2->msgid, payload);
		if (hash == NULL) {
			delph2(iph2);
			goto end;
		}

		/* initialized total buffer length */
		tlen = hash->l;
		tlen += sizeof(*gen);
	} else {
		/* IKE-SA is not established */
		hash = NULL;

		/* initialized total buffer length */
		tlen = 0;
	}
	if ((flags & ISAKMP_FLAG_A) == 0)
		iph2->flags = (hash == NULL ? 0 : ISAKMP_FLAG_E);
	else
		iph2->flags = (hash == NULL ? 0 : ISAKMP_FLAG_A);

	insph2(iph2);
	bindph12(iph1, iph2);

	tlen += sizeof(*isakmp) + payload->l;

	/* create buffer for isakmp payload */
	iph2->sendbuf = vmalloc(tlen);
	if (iph2->sendbuf == NULL) { 
		plog(LLV_ERROR, LOCATION, NULL,
			"failed to get buffer to send.\n");
		goto err;
	}

	/* create isakmp header */
	isakmp = (struct isakmp *)iph2->sendbuf->v;
	memcpy(&isakmp->i_ck, &iph1->index.i_ck, sizeof(cookie_t));
	memcpy(&isakmp->r_ck, &iph1->index.r_ck, sizeof(cookie_t));
	isakmp->np = hash == NULL ? (np & 0xff) : ISAKMP_NPTYPE_HASH;
	isakmp->v = iph1->version;
	isakmp->etype = ISAKMP_ETYPE_CFG;
	isakmp->flags = iph2->flags;
	memcpy(&isakmp->msgid, &iph2->msgid, sizeof(isakmp->msgid));
	isakmp->len = htonl(tlen);
	p = (char *)(isakmp + 1);

	/* create HASH payload */
	if (hash != NULL) {
		gen = (struct isakmp_gen *)p;
		gen->np = np & 0xff;
		gen->len = htons(sizeof(*gen) + hash->l);
		p += sizeof(*gen);
		memcpy(p, hash->v, hash->l);
		p += hash->l;
	}

	/* add payload */
	memcpy(p, payload->v, payload->l);
	p += payload->l;

#ifdef HAVE_PRINT_ISAKMP_C
	isakmp_printpacket(iph2->sendbuf, iph1->local, iph1->remote, 1);
#endif

	/* encoding */
	if (ISSET(isakmp->flags, ISAKMP_FLAG_E)) {
		vchar_t *tmp;

		tmp = oakley_do_encrypt(iph2->ph1, iph2->sendbuf, 
			ics->ivm->ive, ics->ivm->iv);
		VPTRINIT(iph2->sendbuf);
		if (tmp == NULL)
			goto err;
		iph2->sendbuf = tmp;
	}

	/* HDR*, HASH(1), ATTR */
	if (isakmp_send(iph2->ph1, iph2->sendbuf) < 0) {
		VPTRINIT(iph2->sendbuf);
		goto err;
	}

	plog(LLV_DEBUG, LOCATION, NULL,
		"sendto mode config %s.\n", s_isakmp_nptype(np));

	/*
	 * XXX We might need to resend the message...
	 */

	error = 0;
	VPTRINIT(iph2->sendbuf);

err:
	if (iph2->sendbuf != NULL)
		vfree(iph2->sendbuf);

	unbindph12(iph2);
	remph2(iph2);
	delph2(iph2);
end:
	if (hash)
		vfree(hash);
	return error;
}


void 
isakmp_cfg_rmstate(iph1)
	struct ph1handle *iph1;
{
	struct isakmp_cfg_state *state = iph1->mode_cfg;

	if (isakmp_cfg_accounting(iph1, ISAKMP_CFG_LOGOUT) != 0)
		plog(LLV_ERROR, LOCATION, NULL, "Accounting failed\n");

	if (state->flags & ISAKMP_CFG_PORT_ALLOCATED)
		isakmp_cfg_putport(iph1, state->port);	

	xauth_rmstate(&state->xauth);

	racoon_free(state);
	iph1->mode_cfg = NULL;

	return;
}

struct isakmp_cfg_state *
isakmp_cfg_mkstate(void) 
{
	struct isakmp_cfg_state *state;

	if ((state = racoon_malloc(sizeof(*state))) == NULL) {
		plog(LLV_ERROR, LOCATION, NULL,
		    "Cannot allocate memory for mode config state\n");
		return NULL;
	}
	memset(state, 0, sizeof(*state));

	return state;
}

int 
isakmp_cfg_getport(iph1)
	struct ph1handle *iph1;
{
	unsigned int i;
	size_t size = isakmp_cfg_config.pool_size;

	if (iph1->mode_cfg->flags & ISAKMP_CFG_PORT_ALLOCATED)
		return iph1->mode_cfg->port;

	if (isakmp_cfg_config.port_pool == NULL) {
		plog(LLV_ERROR, LOCATION, NULL,
		    "isakmp_cfg_config.port_pool == NULL\n");
		return -1;
	}

	for (i = 0; i < size; i++) {
		if (isakmp_cfg_config.port_pool[i].used == 0)
			break;
	}

	if (i == size) {
		plog(LLV_ERROR, LOCATION, NULL, 
		    "No more addresses available\n");
			return -1;
	}

	isakmp_cfg_config.port_pool[i].used = 1;

	plog(LLV_INFO, LOCATION, NULL, "Using port %d\n", i);

	iph1->mode_cfg->flags |= ISAKMP_CFG_PORT_ALLOCATED;
	iph1->mode_cfg->port = i;

	return i;
}

int 
isakmp_cfg_putport(iph1, index)
	struct ph1handle *iph1;
	unsigned int index;
{
	if (isakmp_cfg_config.port_pool == NULL) {
		plog(LLV_ERROR, LOCATION, NULL, 
		    "isakmp_cfg_config.port_pool == NULL\n");
		return -1;
	}

	if (isakmp_cfg_config.port_pool[index].used == 0) {
		plog(LLV_ERROR, LOCATION, NULL, 
		    "Attempt to release an unallocated address (port %d)\n",
		    index);
		return -1;
	}

#ifdef HAVE_LIBPAM
	/* Cleanup PAM status associated with the port */
	if (isakmp_cfg_config.authsource == ISAKMP_CFG_AUTH_PAM)
		privsep_cleanup_pam(index);
#endif
	isakmp_cfg_config.port_pool[index].used = 0;
	iph1->mode_cfg->flags &= ISAKMP_CFG_PORT_ALLOCATED;

	plog(LLV_INFO, LOCATION, NULL, "Released port %d\n", index);

	return 0;
}

#ifdef HAVE_LIBPAM
void
cleanup_pam(port)
	int port;
{
	if (isakmp_cfg_config.port_pool[port].pam != NULL) {
		pam_end(isakmp_cfg_config.port_pool[port].pam, PAM_SUCCESS);
		isakmp_cfg_config.port_pool[port].pam = NULL;
	}

	return;
}
#endif

/* Accounting, only for RADIUS or PAM */
static int
isakmp_cfg_accounting(iph1, inout)
	struct ph1handle *iph1;
	int inout;
{
#ifdef HAVE_LIBPAM
	if (isakmp_cfg_config.accounting == ISAKMP_CFG_ACCT_PAM)
		return privsep_accounting_pam(iph1->mode_cfg->port, 
		    inout);
#endif 
#ifdef HAVE_LIBRADIUS
	if (isakmp_cfg_config.accounting == ISAKMP_CFG_ACCT_RADIUS)
		return isakmp_cfg_accounting_radius(iph1, inout);
#endif
	return 0;
}

#ifdef HAVE_LIBPAM
int 
isakmp_cfg_accounting_pam(port, inout)
	int port;
	int inout;
{
	int error = 0;
	pam_handle_t *pam;

	if (isakmp_cfg_config.port_pool == NULL) {
		plog(LLV_ERROR, LOCATION, NULL, 
		    "isakmp_cfg_config.port_pool == NULL\n");
		return -1;
	}
	
	pam = isakmp_cfg_config.port_pool[port].pam;
	if (pam == NULL) {
		plog(LLV_ERROR, LOCATION, NULL, "pam handle is NULL\n");
		return -1;
	}

	switch (inout) {
	case ISAKMP_CFG_LOGIN:
		error = pam_open_session(pam, 0);
		break;
	case ISAKMP_CFG_LOGOUT:
		error = pam_close_session(pam, 0);
		pam_end(pam, error);
		isakmp_cfg_config.port_pool[port].pam = NULL;
		break;
	default:
		plog(LLV_ERROR, LOCATION, NULL, "Unepected inout\n");
		break;
	}
	
	if (error != 0) {
		plog(LLV_ERROR, LOCATION, NULL,
		    "pam_open_session/pam_close_session failed: %s\n",
		    pam_strerror(pam, error)); 
		return -1;
        }

	return 0;
}
#endif /* HAVE_LIBPAM */

#ifdef HAVE_LIBRADIUS
static int
isakmp_cfg_accounting_radius(iph1, inout)
	struct ph1handle *iph1;
	int inout;
{
	/* For first time use, initialize Radius */
	if (radius_acct_state == NULL) {
		if ((radius_acct_state = rad_acct_open()) == NULL) {
			plog(LLV_ERROR, LOCATION, NULL,
			    "Cannot init librradius\n");
			return -1;
		}

		if (rad_config(radius_acct_state, NULL) != 0) {
			 plog(LLV_ERROR, LOCATION, NULL,
			     "Cannot open librarius config file: %s\n",
			     rad_strerror(radius_acct_state));
			  rad_close(radius_acct_state);
			  radius_acct_state = NULL;
			  return -1;
		}
	}

	if (rad_create_request(radius_acct_state, 
	    RAD_ACCOUNTING_REQUEST) != 0) {
		plog(LLV_ERROR, LOCATION, NULL,
		    "rad_create_request failed: %s\n",
		    rad_strerror(radius_acct_state));
		return -1;
	}

	if (rad_put_string(radius_acct_state, RAD_USER_NAME, 
	    iph1->mode_cfg->login) != 0) {
		plog(LLV_ERROR, LOCATION, NULL,
		    "rad_put_string failed: %s\n",
		    rad_strerror(radius_acct_state));
		return -1;
	}

	switch (inout) {
	case ISAKMP_CFG_LOGIN:
		inout = RAD_START;
		break;
	case ISAKMP_CFG_LOGOUT:
		inout = RAD_STOP;
		break;
	default:
		plog(LLV_ERROR, LOCATION, NULL, "Unepected inout\n");
		break;
	}

	if (rad_put_addr(radius_acct_state, 
	    RAD_FRAMED_IP_ADDRESS, iph1->mode_cfg->addr4) != 0) {
		plog(LLV_ERROR, LOCATION, NULL,
		    "rad_put_addr failed: %s\n",
		    rad_strerror(radius_acct_state));
		return -1;
	}

	if (rad_put_addr(radius_acct_state, 
	    RAD_LOGIN_IP_HOST, iph1->mode_cfg->addr4) != 0) {
		plog(LLV_ERROR, LOCATION, NULL,
		    "rad_put_addr failed: %s\n",
		    rad_strerror(radius_acct_state));
		return -1;
	}

	if (rad_put_int(radius_acct_state, RAD_ACCT_STATUS_TYPE, inout) != 0) {
		plog(LLV_ERROR, LOCATION, NULL,
		    "rad_put_int failed: %s\n",
		    rad_strerror(radius_acct_state));
		return -1;
	}

	if (isakmp_cfg_radius_common(radius_acct_state, 
	    iph1->mode_cfg->port) != 0)
		return -1;

	if (rad_send_request(radius_acct_state) != RAD_ACCOUNTING_RESPONSE) {
		plog(LLV_ERROR, LOCATION, NULL,
		    "rad_send_request failed: %s\n",
		    rad_strerror(radius_acct_state));
		return -1;
	}

	return 0;
}
#endif /* HAVE_LIBRADIUS */

/*
 * Attributes common to all RADIUS requests
 */
#ifdef HAVE_LIBRADIUS
int
isakmp_cfg_radius_common(radius_state, port)
	struct rad_handle *radius_state;
	int port;
{ 
	struct utsname name;
	static struct hostent *host = NULL;
	struct in_addr nas_addr;

	/* 
	 * Find our own IP by resolving our nodename
	 */
	if (host == NULL) {
		if (uname(&name) != 0) {
			plog(LLV_ERROR, LOCATION, NULL,
			    "uname failed: %s\n", strerror(errno));
			return -1;
		}

		if ((host = gethostbyname(name.nodename)) == NULL) {
			plog(LLV_ERROR, LOCATION, NULL,
			    "gethostbyname failed: %s\n", strerror(errno));
			return -1;
		}
	}

	memcpy(&nas_addr, host->h_addr, sizeof(nas_addr));
	if (rad_put_addr(radius_state, RAD_NAS_IP_ADDRESS, nas_addr) != 0) {
		plog(LLV_ERROR, LOCATION, NULL,
		    "rad_put_addr failed: %s\n",
		    rad_strerror(radius_state));
		return -1;
	}

	if (rad_put_int(radius_state, RAD_NAS_PORT, port) != 0) {
		plog(LLV_ERROR, LOCATION, NULL,
		    "rad_put_int failed: %s\n",
		    rad_strerror(radius_state));
		return -1;
	}

	if (rad_put_int(radius_state, RAD_NAS_PORT_TYPE, RAD_VIRTUAL) != 0) {
		plog(LLV_ERROR, LOCATION, NULL,
		    "rad_put_int failed: %s\n",
		    rad_strerror(radius_state));
		return -1;
	}

	if (rad_put_int(radius_state, RAD_SERVICE_TYPE, RAD_FRAMED) != 0) {
		plog(LLV_ERROR, LOCATION, NULL,
		    "rad_put_int failed: %s\n",
		    rad_strerror(radius_state));
		return -1;
	}
	
	return 0;
}
#endif
	
int 
isakmp_cfg_getconfig(iph1)
	struct ph1handle *iph1;
{
	vchar_t *buffer;
	struct isakmp_pl_attr *attrpl;
	struct isakmp_data *attr;
	size_t len;
	int error;
	int attrcount;
	int i;
	int attrlist[] = {
		INTERNAL_IP4_ADDRESS,
		INTERNAL_IP4_NETMASK,
		INTERNAL_IP4_DNS,
		INTERNAL_IP4_NBNS,
		UNITY_BANNER,
		APPLICATION_VERSION,
	};

	attrcount = sizeof(attrlist) / sizeof(*attrlist);
	len = sizeof(*attrpl) + sizeof(*attr) * attrcount;
	    
	if ((buffer = vmalloc(len)) == NULL) {
		plog(LLV_ERROR, LOCATION, NULL, "Cannot allocate memory\n");
		return -1;
	}

	attrpl = (struct isakmp_pl_attr *)buffer->v;
	attrpl->h.len = htons(len);
	attrpl->type = ISAKMP_CFG_REQUEST;
	attrpl->id = htons((u_int16_t)(eay_random() & 0xffff));

	attr = (struct isakmp_data *)(attrpl + 1);

	for (i = 0; i < attrcount; i++) {
		attr->type = htons(attrlist[i]);
		attr->lorv = htons(0);
		attr++;
	}

	error = isakmp_cfg_send(iph1, buffer,
	    ISAKMP_NPTYPE_ATTR, ISAKMP_FLAG_E, 1);

	vfree(buffer);

	return error;
}

static void
isakmp_cfg_getaddr4(attr, ip)
	struct isakmp_data *attr;
	struct in_addr *ip;
{
	size_t alen = ntohs(attr->lorv);
	in_addr_t *addr;

	if (alen != sizeof(*ip)) {
		plog(LLV_ERROR, LOCATION, NULL, "Bad IPv4 address len\n");
		return;
	}

	addr = (in_addr_t *)(attr + 1);
	ip->s_addr = *addr;

	return;
}

int
isakmp_cfg_setenv(iph1, envp, envc)
	struct ph1handle *iph1; 
	char ***envp;
	int *envc;
{
#define IP_MAX 40
	char addrstr[IP_MAX];

	/* 
	 * Internal IPv4 address, either if 
	 * we are a client or a server.
	 */
	if ((iph1->mode_cfg->flags & ISAKMP_CFG_GOT_ADDR4) ||
#ifdef HAVE_LIBRADIUS
	    (iph1->mode_cfg->flags & ISAKMP_CFG_ADDR4_RADIUS) ||
#endif
	    (iph1->mode_cfg->flags & ISAKMP_CFG_ADDR4_LOCAL)) {
		inet_ntop(AF_INET, &iph1->mode_cfg->addr4, 
		    addrstr, IP_MAX);
	} else
		addrstr[0] = '\0';

	if (script_env_append(envp, envc, "INTERNAL_ADDR4", addrstr) != 0) {
		plog(LLV_ERROR, LOCATION, NULL, "Cannot set INTERNAL_ADDR4\n");
		return -1;
	}

	/* Internal IPv4 mask */
	if (iph1->mode_cfg->flags & ISAKMP_CFG_GOT_MASK4) 
		inet_ntop(AF_INET, &iph1->mode_cfg->mask4, 
		    addrstr, IP_MAX);
	else
		addrstr[0] = '\0';

       /* 
	* During several releases, documentation adverised INTERNAL_NETMASK4
	* while code was using INTERNAL_MASK4. We now do both.
	*/
	if (script_env_append(envp, envc, "INTERNAL_MASK4", addrstr) != 0) {
                plog(LLV_ERROR, LOCATION, NULL, "Cannot set INTERNAL_MASK4\n");
                return -1;
        } 

       if (script_env_append(envp, envc, "INTERNAL_NETMASK4", addrstr) != 0) {
	       plog(LLV_ERROR, LOCATION, NULL,  
		   "Cannot set INTERNAL_NETMASK4\n");
	       return -1;
       }


	/* Internal IPv4 DNS */
	if (iph1->mode_cfg->flags & ISAKMP_CFG_GOT_DNS4) 
		inet_ntop(AF_INET, &iph1->mode_cfg->dns4, 
		    addrstr, IP_MAX);
	else
		addrstr[0] = '\0';

	if (script_env_append(envp, envc, "INTERNAL_DNS4", addrstr) != 0) { 
		plog(LLV_ERROR, LOCATION, NULL, "Cannot set INTERNAL_DNS4\n");
		return -1;
	}

	/* Internal IPv4 WINS */
	if (iph1->mode_cfg->flags & ISAKMP_CFG_GOT_WINS4) 
		inet_ntop(AF_INET, &iph1->mode_cfg->wins4, 
		    addrstr, IP_MAX);
	else
		addrstr[0] = '\0';

	if (script_env_append(envp, envc, "INTERNAL_WINS4", addrstr) != 0) { 
		plog(LLV_ERROR, LOCATION, NULL, "Cannot set INTERNAL_WINS4\n");
		return -1;
	}

	return 0;
}