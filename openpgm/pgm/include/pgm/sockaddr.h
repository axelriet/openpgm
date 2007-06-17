/* vim:ts=8:sts=4:sw=4:noai:noexpandtab
 * 
 * struct sockaddr functions independent of in or in6.
 *
 * Copyright (c) 2006-2007 Miru Limited.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef __PGM_SOCKADDR_H__
#define __PGM_SOCKADDR_H__

#include <errno.h>


/* TODO: update to RFC 3678: MCAST_JOIN_(SOURCE_)GROUP */

/* Combination of ip_mreq & ipv6_mreq */
struct sock_mreq
{
/* Port number and IP multicast address of group. */
    struct sockaddr_storage	smr_multiaddr;

/* Local IP address of interface. */
    struct sockaddr_storage	smr_interface;
};

G_BEGIN_DECLS


#define sockaddr_family(src)	( ((struct sockaddr*)(src))->sa_family )

#define sockaddr_port(src) \
	    ( sockaddr_family(src) == AF_INET ? \
		((struct sockaddr_in*)(src))->sin_port : \
		((struct sockaddr_in6*)(src))->sin6_port )

#define sockaddr_addr(src) \
	    ( sockaddr_family(src) == AF_INET ? \
		(const void*)&((struct sockaddr_in*)(src))->sin_addr : \
		(const void*)&((struct sockaddr_in6*)(src))->sin6_addr )

#define sockaddr_len(src) \
	    ( sockaddr_family(src) == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6) )

#define sockaddr_ntop(src,dst,cnt) \
	    ( inet_ntop(sockaddr_family(src), sockaddr_addr(src), (dst), (cnt)) )

static inline int sockaddr_is_addr_multicast (struct sockaddr* s)
{
    int retval = 0;

    switch (s->sa_family) {
    case AF_INET:
	retval = IN_MULTICAST(g_htonl( ((struct sockaddr_in*)s)->sin_addr.s_addr ));
	break;

    case AF_INET6:
	retval = IN6_IS_ADDR_MULTICAST( &((struct sockaddr_in6*)s)->sin6_addr );
	break;

    default:
	retval = -EINVAL;
	break;
    }

    return retval;
}

static inline int sockaddr_cmp (struct sockaddr *a, struct sockaddr *b)
{
    int retval = 0;

    if (a->sa_family != b->sa_family)
    {
	retval = a->sa_family < b->sa_family ? -1 : 1;
    }
    else
    {
	switch (a->sa_family) {
	case AF_INET:
	    if (((struct sockaddr_in*)a)->sin_addr.s_addr != ((struct sockaddr_in*)b)->sin_addr.s_addr)
	    {
		retval = ((struct sockaddr_in*)a)->sin_addr.s_addr < ((struct sockaddr_in*)b)->sin_addr.s_addr ? -1 : 1;
	    }
	    break;

	case AF_INET6:
	    retval = memcmp (&((struct sockaddr_in6*)a)->sin6_addr, &((struct sockaddr_in6*)b)->sin6_addr, sizeof(struct in6_addr));
	    break;

	default:
	    retval = -EINVAL;
	    break;
	}
    }

    return retval;
}

static inline int sockaddr_hdrincl (int s, int sa_family, gboolean v)
{
    int retval = 0;

    switch (sa_family) {
    case AF_INET:
	retval = setsockopt (s, IPPROTO_IP, IP_HDRINCL, &v, sizeof(v));
	break;

    case AF_INET6:  /* method does not exist */
	break;

    default:
	retval = -EINVAL;
	break;
    }

    return retval;
}

static inline int sockaddr_router_alert (int s, int sa_family, gboolean v)
{
    int retval = 0;

    switch (sa_family) {
    case AF_INET:
	retval = setsockopt (s, IPPROTO_IP, IP_ROUTER_ALERT, &v, sizeof(v));
	break;

    case AF_INET6:
	retval = setsockopt (s, IPPROTO_IPV6, IPV6_ROUTER_ALERT, &v, sizeof(v));
	break;

    default:
	retval = -EINVAL;
	break;
    }

    return retval;
}

static inline int sockaddr_tos (int s, int sa_family, int tos)
{
    int retval = 0;

    switch (sa_family) {
    case AF_INET:
	retval = setsockopt (s, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
	break;

    case AF_INET6:  /* TRAFFIC_CLASS not implemented */
	break;

    default:
	retval = -EINVAL;
	break;
    }

    return retval;
}

/* nb: IPV6_JOIN_GROUP == IPV6_ADD_MEMBERSHIP
 */
static inline int sockaddr_add_membership (int s, struct sock_mreq* smr)
{
    int retval = 0;

    switch (sockaddr_family(&smr->smr_interface)) {
    case AF_INET:
	{
/* Linux: ip_mreqn preferred, ip_mreq supported for compat */
	    struct ip_mreq mreq;
	    memset (&mreq, 0, sizeof(mreq));

	    mreq.imr_multiaddr.s_addr = ((struct sockaddr_in*)&smr->smr_multiaddr)->sin_addr.s_addr;
	    mreq.imr_interface.s_addr = ((struct sockaddr_in*)&smr->smr_interface)->sin_addr.s_addr;

	    retval = setsockopt (s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
	}
	break;

    case AF_INET6:
	{
	    struct ipv6_mreq mreq6;
	    memset (&mreq6, 0, sizeof(mreq6));

	    memcpy (&mreq6.ipv6mr_multiaddr, &((struct sockaddr_in6*)&smr->smr_multiaddr)->sin6_addr, sizeof(struct in6_addr));
	    memcpy (&mreq6.ipv6mr_interface, &((struct sockaddr_in6*)&smr->smr_interface)->sin6_addr, sizeof(struct in6_addr));

	    retval = setsockopt (s, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq6, sizeof(mreq6));
	}
	break;

    default:
	retval = -EINVAL;
	break;
    }

    return retval;
}

static inline int sockaddr_multicast_if (int s, struct sock_mreq* smr)
{
    int retval = 0;

    switch (sockaddr_family(&smr->smr_interface)) {
    case AF_INET:
	{
/* Linux: ip_mreqn or ip_mreq, many Unix just support in_addr (interface/address) */
	    struct ip_mreq mreq;
	    memset (&mreq, 0, sizeof(mreq));

	    mreq.imr_multiaddr.s_addr = ((struct sockaddr_in*)&smr->smr_multiaddr)->sin_addr.s_addr;
	    mreq.imr_interface.s_addr = ((struct sockaddr_in*)&smr->smr_interface)->sin_addr.s_addr;

//	    retval = setsockopt (s, IPPROTO_IP, IP_MULTICAST_IF, &mreq, sizeof(mreq));
	    retval = setsockopt (s, IPPROTO_IP, IP_MULTICAST_IF, &mreq.imr_interface, sizeof(mreq.imr_interface));
	}
	break;

    case AF_INET6:
	{
	    int interface_index = 0;

/* TODO: calculate index or pass in from above and also use with mreqn */

	    retval = setsockopt (s, IPPROTO_IPV6, IPV6_MULTICAST_IF, &interface_index, sizeof(interface_index));
	}
	break;

    default:
	retval = -EINVAL;
	break;
    }

    return retval;
}

static inline int sockaddr_multicast_loop (int s, int sa_family, gboolean v)
{
    int retval = 0;

    switch (sa_family) {
    case AF_INET:
	retval = setsockopt (s, IPPROTO_IP, IP_MULTICAST_LOOP, &v, sizeof(v));
	break;

    case AF_INET6:
	retval = setsockopt (s, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &v, sizeof(v));
	break;

    default:
	retval = -EINVAL;
	break;
    }

    return retval;
}

static inline int sockaddr_multicast_hops (int s, int sa_family, gint hops)
{
    int retval = 0;

    switch (sa_family) {
    case AF_INET:
	retval = setsockopt (s, IPPROTO_IP, IP_MULTICAST_TTL, &hops, sizeof(hops));
	break;

    case AF_INET6:
	retval = setsockopt (s, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops));
	break;

    default:
	retval = -EINVAL;
	break;
    }

    return retval;
}


G_END_DECLS

#endif /* __PGM_SOCKADDR_H__ */
