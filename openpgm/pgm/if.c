/* vim:ts=8:sts=8:sw=4:noai:noexpandtab
 *
 * network interface handling.
 *
 * Copyright (c) 2006-2012 Miru Limited.
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

#ifdef HAVE_CONFIG_H
#	include <config.h>
#endif

#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif

#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#ifndef _WIN32
#ifdef _AIX
#       define IP_MULTICAST
#endif
#	include <arpa/inet.h>
#	include <sys/types.h>
#	include <sys/socket.h>
#	include <netdb.h>		/* _GNU_SOURCE for EAI_NODATA */
#endif
#include <impl/i18n.h>
#include <impl/framework.h>
#include <impl/inet_lnaof.h>
#include <pgm/if.h>


//#define IF_DEBUG

/* temporary structure to contain interface name whilst address family
 * has not been resolved.
 */
struct interface_req {
	char			ir_name[IF_NAMESIZE];
	unsigned int		ir_flags;		/* from SIOCGIFFLAGS */
	unsigned int		ir_interface;		/* interface index */
	struct sockaddr_storage ir_addr;		/* interface address */
};

#ifdef _MSC_VER
/* avoid unsupported VLAs in MSVC 2013 */
#	define IR_STRLEN	1024
#else
#	define IR_STRLEN	(IF_NAMESIZE + INET6_ADDRSTRLEN + strlen ("ir_name: \"\", ir_flags: \"\", ir_interface: , ir_addr: \"\"") + strlen ("4294967295") + strlen ("UP,LOOPBACK,BROADCAST,MULTICAST"))
#endif


/* locals */

#ifndef _WIN32
#	define IF_DEFAULT_GROUP	((in_addr_t)0xefc00001) /* 239.192.0.1 */
#else
#	define IF_DEFAULT_GROUP	((u_long)0xefc00001)
#endif

/* ff08::1 */
#ifdef _AIX
#define IF6_DEFAULT_INIT { { .u6_addr8 = { 0xff,8,0,0,0,0,0,0,0,0,0,0,0,0,0,1 } } }
#else
#define IF6_DEFAULT_INIT { { { 0xff,8,0,0,0,0,0,0,0,0,0,0,0,0,0,1 } } }
#endif
const struct in6_addr if6_default_group_addr = IF6_DEFAULT_INIT;


static inline bool is_in_net (const struct in_addr*restrict, const struct in_addr*restrict, const struct in_addr*restrict) PGM_GNUC_WARN_UNUSED_RESULT;
static inline bool is_in_net6 (const struct in6_addr*restrict, const struct in6_addr*restrict, const struct in6_addr*restrict) PGM_GNUC_WARN_UNUSED_RESULT;
static inline bool is_network_char (const int, const char) PGM_GNUC_CONST;


/* recommended address space for multicast:
 * rfc4607, rfc3180, rfc2365
 *
 * avoid 5 high-order bit overlap.
 *
 * loopback:  ffx1::/16
 * segment:   ffx2::/16
 * glop:      238/8
 * mysterious admin:  239/8,   ffx6::/16
 * site:      239.252-255/16,  ffx5::/16
 * org:       239.192/14,      ffx8::/16
 *
 * internets: 224.0.1.0-238.255.255.255,  ffxe::/16
 */

static
char*
ifa_flags_to_string (
	unsigned int flags,
	char* text
	)
{
	if (0 != (flags & IFF_UP))
		strcpy (text, "UP");
	else
		*text = 0;
	if (0 != (flags & IFF_LOOPBACK)) {
		if (*text)
			strcat (text, ",LOOPBACK");
		else
			strcpy (text, "LOOPBACK");
	}
	if (0 != (flags & IFF_BROADCAST)) {
		if (*text)
			strcat (text, ",BROADCAST");
		else
			strcpy (text, "BROADCAST");
	}
	if (0 != (flags & IFF_MULTICAST)) {
		if (*text)
			strcat (text, ",MULTICAST");
		else
			strcpy (text, "MULTICAST");
	}
	return text;
}

static
char*
interface_req_to_string (
	struct interface_req* ir,
	char* text,
	size_t len
	)
{
	char flags[1024];
	char addr[INET6_ADDRSTRLEN];

	if (0 != pgm_sockaddr_ntop ((struct sockaddr*)&ir->ir_addr, addr, sizeof (addr)))
		addr[0] = 0;
	pgm_snprintf_s (text, len, _TRUNCATE, "if_name: \"%s\", ir_flags: \"%s\", ir_interface: %d, ir_addr: \"%s\"",
		ir->ir_name,
		ifa_flags_to_string (ir->ir_flags, flags),
		ir->ir_interface,
		addr);
	return text;
}

char*
pgm_gsr_to_string (
	const struct pgm_group_source_req* gsr,
	char* text,
	size_t len	/* capacity of text buffer */
	)
{
	char group[1024], source[1024], addr[1024];
	if (0 != pgm_sockaddr_ntop ((const struct sockaddr*)&gsr->gsr_group, group, sizeof (group)))
		group[0] = 0;
	if (0 != pgm_sockaddr_ntop ((const struct sockaddr*)&gsr->gsr_source, source, sizeof (source)))
		source[0] = 0;
	if (0 != pgm_sockaddr_ntop ((const struct sockaddr*)&gsr->gsr_addr, addr, sizeof (addr)))
		addr[0] = 0;
	pgm_snprintf_s (text, len, _TRUNCATE, "gsr_interface = %u, gsr_group = \"%s\", gsr_source = \"%s\", gsr_addr = \"%s\"",
		gsr->gsr_interface, group, source, addr);
	return text;
}

char*
pgm_addrinfo_to_string (
	const struct pgm_addrinfo_t* addr,
	char* text,
	size_t len
	)
{
	char gsr[1024], recv_addrs[1024], send_addrs[1024];
	recv_addrs[0] = send_addrs[0] = 0;
	for (unsigned i = 0; i < addr->ai_recv_addrs_len; i++) {
		if (i == 0) strcat (recv_addrs, "{ ");
		else strcat (recv_addrs, ", { ");
		strcat (recv_addrs, pgm_gsr_to_string (&addr->ai_recv_addrs[i], gsr, sizeof (gsr)));
		strcat (recv_addrs, " }");
	}
	for (unsigned i = 0; i < addr->ai_send_addrs_len; i++) {
		if (i == 0) strcat (send_addrs, "{ ");
		else strcat (send_addrs, ", { ");
		strcat (send_addrs, pgm_gsr_to_string (&addr->ai_send_addrs[i], gsr, sizeof (gsr)));
		strcat (send_addrs, " }");
	}
	pgm_snprintf_s (text, len, _TRUNCATE, "ai_family = \"%s\", ai_recv_addrs = [%s], ai_send_addrs = [%s]",
		pgm_family_string (addr->ai_family), recv_addrs, send_addrs);
	return text;
}

/* dump all interfaces to console.
 *
 * note that interface indexes are only in regard to the link layer and hence
 * no 1-1 mapping between adapter name to index back to address.
 */

void
pgm_if_print_all (void)
{
	struct pgm_ifaddrs_t *ifap, *ifa;
	struct pgm_addrinfo_t* res = NULL;

	if (!pgm_getifaddrs (&ifap, NULL))
		return;

	pgm_info (_("IP Configuration"));

	for (ifa = ifap; ifa; ifa = ifa->ifa_next)
	{
/* no address */
		if (NULL == ifa->ifa_addr ||
		     (ifa->ifa_addr->sa_family != AF_INET && 
		      ifa->ifa_addr->sa_family != AF_INET6) )
		{
			continue;
		}

/* interface index */
		const unsigned int idx = NULL == ifa->ifa_addr ? 0 : pgm_if_nametoindex (ifa->ifa_addr->sa_family, ifa->ifa_name);

/* decode flags */
		char flags[1024];
		pgm_info (_("%s: index=%u flags=%u<%s>"),
			ifa->ifa_name ? ifa->ifa_name : "(null)",
			idx,
			ifa->ifa_flags, ifa_flags_to_string (ifa->ifa_flags, flags));

		char addr[INET6_ADDRSTRLEN];
		getnameinfo (ifa->ifa_addr, pgm_sockaddr_len (ifa->ifa_addr),
			     addr, sizeof (addr),
			     NULL, 0,
			     NI_NUMERICHOST);

		if (AF_INET6 == ifa->ifa_addr->sa_family) {
			pgm_info (_("\tinet6 %s prefixlen %u scopeid 0x%x"),
				addr,
				(unsigned)pgm_sockaddr_prefixlen (ifa->ifa_netmask),
				(unsigned)pgm_sockaddr_scope_id (ifa->ifa_addr));
		} else {
			char netmask[INET_ADDRSTRLEN];
			getnameinfo (ifa->ifa_netmask, pgm_sockaddr_len (ifa->ifa_netmask),
				     netmask, sizeof (netmask),
				     NULL, 0,
				     NI_NUMERICHOST);
			pgm_info (_("\tinet %s netmask %s"),
				addr,
				netmask);
		}
	}

	pgm_freeifaddrs (ifap);

/* discover default network parameter */
	if (pgm_getaddrinfo ("", NULL, &res, NULL))
	{
		char network[INET6_ADDRSTRLEN], group[INET6_ADDRSTRLEN];
		struct sockaddr_storage ifaddr;
		struct sockaddr* addr = (struct sockaddr*)&res->ai_recv_addrs[0].gsr_group;
		pgm_get_multicast_enabled_node_addr (addr->sa_family, (struct sockaddr*)&ifaddr, sizeof (ifaddr), NULL);
		pgm_sockaddr_ntop ((struct sockaddr*)&ifaddr, network, sizeof (network));
		if (AF_INET == addr->sa_family) {
			struct sockaddr_in sin;
			memset (&sin, 0, sizeof (sin));
			sin.sin_family = AF_INET;
			sin.sin_addr.s_addr = pgm_htonl (IF_DEFAULT_GROUP);
			memcpy (&ifaddr, &sin, sizeof (sin));
		} else if (AF_INET6 == addr->sa_family) {
			struct sockaddr_in6 sin6;
			memset (&sin6, 0, sizeof (sin6));
			sin6.sin6_family = AF_INET6;
			memcpy (&sin6.sin6_addr, &if6_default_group_addr, sizeof(if6_default_group_addr));
			memcpy (&ifaddr, &sin6, sizeof (sin6));
		} else
			memset (&ifaddr, 0, sizeof (ifaddr));
		pgm_sockaddr_ntop ((struct sockaddr*)&ifaddr, group, sizeof (group));
		pgm_info (_("Default network: \"%s;%s\""), network, group);
		pgm_freeaddrinfo (res);
	}
	else
		pgm_warn (_("Failed to discover default network parameters, verify hostname configuration."));
}

static inline
bool
is_in_net (
	const struct in_addr* restrict	addr,		/* host byte order */
	const struct in_addr* restrict	netaddr,
	const struct in_addr* restrict	netmask
	)
{
	pgm_assert (NULL != addr);
	pgm_assert (NULL != netaddr);
	pgm_assert (NULL != netmask);

#ifdef IF_DEBUG
	const struct in_addr taddr    = { .s_addr = pgm_htonl (addr->s_addr) };
	const struct in_addr tnetaddr = { .s_addr = pgm_htonl (netaddr->s_addr) };
	const struct in_addr tnetmask = { .s_addr = pgm_htonl (netmask->s_addr) };
	char saddr[INET_ADDRSTRLEN], snetaddr[INET_ADDRSTRLEN], snetmask[INET_ADDRSTRLEN];
	pgm_debug ("is_in_net (addr:%s netaddr:%s netmask:%s)",
		 pgm_inet_ntop (AF_INET, &taddr,    saddr,    sizeof(saddr)),
		 pgm_inet_ntop (AF_INET, &tnetaddr, snetaddr, sizeof(snetaddr)),
		 pgm_inet_ntop (AF_INET, &tnetmask, snetmask, sizeof(snetmask)));
#endif

	if ((addr->s_addr & netmask->s_addr) == (netaddr->s_addr & netmask->s_addr))
		return TRUE;
	return FALSE;
}

static
bool
is_in_net6 (
	const struct in6_addr* restrict	addr,
	const struct in6_addr* restrict	netaddr,
	const struct in6_addr* restrict	netmask
	)
{
	pgm_assert (NULL != addr);
	pgm_assert (NULL != netaddr);
	pgm_assert (NULL != netmask);

#ifdef IF_DEBUG
	char saddr[INET6_ADDRSTRLEN], snetaddr[INET6_ADDRSTRLEN], snetmask[INET6_ADDRSTRLEN];
	pgm_debug ("is_in_net6 (addr:%s netaddr:%s netmask:%s)",
		 pgm_inet_ntop (AF_INET6, addr, saddr, sizeof(saddr)),
		 pgm_inet_ntop (AF_INET6, netaddr, snetaddr, sizeof(snetaddr)),
		 pgm_inet_ntop (AF_INET6, netmask, snetmask, sizeof(snetmask)));
#endif

	for (unsigned i = 0; i < 16; i++)
		if ((addr->s6_addr[i] & netmask->s6_addr[i]) != (netaddr->s6_addr[i] & netmask->s6_addr[i]))
			return FALSE;
	return TRUE;
}

/* parse interface entity into an interface-request structure.
 *
 * e.g.  eth0
 *       1.2.3.4
 *       1.2
 *       abcd::
 *       [abcd::]
 *       <hostname>
 *       <nss network name>
 *
 * special addresses should be ignored:
 *
 * local physical link: 169.254.0.0/16, fe80::/64
 * broadcast: 255.255.255.255
 * multicast: 224.0.0.0/4 (224.0.0.0 to 239.255.255.255), ff00::/8
 *
 * We could use if_nametoindex() but we might as well check that the interface is
 * actually UP and capable of multicast traffic.
 *
 * returns TRUE on success, FALSE on error and sets error appropriately.
 */

static
bool
parse_interface (
	int				family,			/* AF_UNSPEC | AF_INET | AF_INET6 */
	const char*	      restrict	ifname,			/* NULL terminated */
	struct interface_req* restrict	ir,			/* location to write interface details to */
	pgm_error_t**	      restrict	error
	)
{
	bool check_inet_network = FALSE, check_inet6_network = FALSE;
	bool check_addr = FALSE;
	bool check_ifname = FALSE;
	char literal[1024];
	struct in_addr in_addr;
	struct sockaddr_in6 sa6_addr;
	struct pgm_ifaddrs_t *ifap, *ifa;
	struct sockaddr_storage addr_storage, *addr = &addr_storage;
	unsigned addr_cnt = 1, interface_matches = 0;

/* pre-conditions */
	pgm_assert (AF_INET == family || AF_INET6 == family || AF_UNSPEC == family);
	pgm_assert (NULL != ifname);
	pgm_assert (NULL != ir);

	pgm_debug ("parse_interface (family:%s ifname:%s%s%s ir:%p error:%p)",
		pgm_family_string (family),
		ifname ? "\"" : "", ifname ? ifname : "(null)", ifname ? "\"" : "",
		(const void*)ir,
		(const void*)error);

/* Strip any square brackets for IPv6 early evaluation.  RFC 2732 defines the
 * term "ipv6reference" for a IPv6 literal address enclosed in square brackets.
 */
	if (AF_INET != family &&
	    '[' == ifname[0])
	{
		const size_t ifnamelen = strlen (ifname);
		if (']' == ifname[ ifnamelen - 1 ]) {
			pgm_strncpy_s (literal, sizeof (literal), ifname + 1, ifnamelen - 2);
			pgm_debug ("Attempting to parse interface as IPv6 literal address \"%s\".", literal);
			family = AF_INET6;		/* force IPv6 evaluation */
			check_inet6_network = TRUE;	/* may be a network IP or CIDR block */
			check_addr = TRUE;		/* cannot be not a name */
			ifname = literal;
		}
	}

/* Network address: e.g. 172.16.0.0, fc00::, or even fec0::%qe0 for scope.
 * For IPv4 use compatibiltiy API with in_addr in host byte order.
 */
	if (AF_INET6 != family && 0 == pgm_inet_network (ifname, &in_addr))
	{
#ifdef IF_DEBUG
		struct in_addr t = { .s_addr = pgm_htonl (in_addr.s_addr) };
		pgm_debug ("IPv4 network address: %s", inet_ntoa (t));
#endif
		if (IN_MULTICAST(in_addr.s_addr)) {
			pgm_set_error (error,
				     PGM_ERROR_DOMAIN_IF,
				     PGM_ERROR_XDEV,
				     _("Expecting network interface address, found IPv4 multicast network %s%s%s"),
				     ifname ? "\"" : "", ifname ? ifname : "(null)", ifname ? "\"" : "");
			if (NULL != error)
				pgm_debug ("parse_interface() failed: %s", (*error)->message);
			return FALSE;
		}
/* promote to sockaddr and avoid type punning */
		struct sockaddr_in s4;
		memset (&s4, 0, sizeof(s4));
		s4.sin_family = AF_INET;
		s4.sin_addr.s_addr = pgm_htonl (in_addr.s_addr);
		memcpy (addr, &s4, sizeof(s4));

		check_inet_network = TRUE;
		check_addr = TRUE;
	}
/* For IPv6 use an internal API that mimicks inet_network but works with sockaddr
 * instead of in6_addr, the promotion is required to save the scope identifier.
 */
	if (AF_INET != family && 0 == pgm_sa6_network (ifname, &sa6_addr))
	{
		if (IN6_IS_ADDR_MULTICAST(&sa6_addr.sin6_addr)) {
			pgm_set_error (error,
				     PGM_ERROR_DOMAIN_IF,
				     PGM_ERROR_XDEV,
				     _("Expecting network interface address, found IPv6 multicast network %s%s%s"),
				     ifname ? "\"" : "", ifname ? ifname : "(null)", ifname ? "\"" : "");
			if (NULL != error)
				pgm_debug ("parse_interface() failed: %s", (*error)->message);
			return FALSE;
		}
		memcpy (addr, &sa6_addr, sizeof (sa6_addr));

		check_inet6_network = TRUE;
		check_addr = TRUE;
	}

/* numeric host with scope id, e.g. abcd::1%eth0 */
	if (!check_addr)
	{
		char errbuf[1024];
		struct addrinfo hints = {
			.ai_family	= family,
			.ai_socktype	= SOCK_STREAM,				/* not really, SOCK_RAW */
			.ai_protocol	= IPPROTO_TCP,				/* not really, IPPROTO_PGM */
			.ai_flags	= AI_ADDRCONFIG | AI_NUMERICHOST	/* AI_V4MAPPED is unhelpful */
		}, *res;
		const int eai = getaddrinfo (ifname, NULL, &hints, &res);
		switch (eai) {
		case 0:
			if (AF_INET == res->ai_family &&
			    IN_MULTICAST(pgm_ntohl (((struct sockaddr_in*)(res->ai_addr))->sin_addr.s_addr)))
			{
				pgm_set_error (error,
					     PGM_ERROR_DOMAIN_IF,
					     PGM_ERROR_XDEV,
					     _("Expecting interface address, found IPv4 multicast address %s%s%s"),
					     ifname ? "\"" : "", ifname ? ifname : "(null)", ifname ? "\"" : "");
				freeaddrinfo (res);
				if (NULL != error)
					pgm_debug ("parse_interface() failed: %s", (*error)->message);
				return FALSE;
			}
			else if (AF_INET6 == res->ai_family &&
				 IN6_IS_ADDR_MULTICAST(&((struct sockaddr_in6*)res->ai_addr)->sin6_addr))
			{
				pgm_set_error (error,
					     PGM_ERROR_DOMAIN_IF,
					     PGM_ERROR_XDEV,
					     _("Expecting interface address, found IPv6 multicast address %s%s%s"),
					     ifname ? "\"" : "", ifname ? ifname : "(null)", ifname ? "\"" : "");
				freeaddrinfo (res);
				if (NULL != error)
					pgm_debug ("parse_interface() failed: %s", (*error)->message);
				return FALSE;
			}

			memcpy (addr, res->ai_addr, pgm_sockaddr_len (res->ai_addr));
			freeaddrinfo (res);
			check_addr = TRUE;
			break;

#if defined(EAI_NODATA) && EAI_NODATA != EAI_NONAME
		case EAI_NODATA:
#endif
		case EAI_NONAME:
			break;

		default:
			pgm_set_error (error,
				     PGM_ERROR_DOMAIN_IF,
				     pgm_error_from_eai_errno (eai, errno),
				     _("Numeric host resolution: %s(%d)"),
				     pgm_gai_strerror_s (errbuf, sizeof (errbuf), eai),
				     eai);
			if (NULL != error)
				pgm_debug ("parse_interface() failed: %s", (*error)->message);
			return FALSE;
		}
	}

/* Network name into network address, can be expensive with NSS network lookup.
 * Limitation as per the man page NETWORKS(5):
 *
 * Only Class A, B or C networks are supported, partitioned networks
 * (i.e. network/26 or network/28) are not supported by this facility.
 */
	if (!(check_inet_network || check_inet6_network))
	{
		const struct pgm_netent_t* ne = pgm_getnetbyname (ifname);
/* ne::n_net in host byte order */

		if (ne) {
			switch (ne->n_net.ss_family) {
			case AF_INET: {
				struct sockaddr_in sa;
				if (AF_INET6 == family) {
					pgm_set_error (error,
						     PGM_ERROR_DOMAIN_IF,
						     PGM_ERROR_NODEV,
						     _("IP address family conflict when resolving network name %s%s%s, found AF_INET when AF_INET6 expected."),
						     ifname ? "\"" : "", ifname ? ifname : "(null)", ifname ? "\"" : "");
					if (NULL != error)
						pgm_debug ("parse_interface() failed: %s", (*error)->message);
					return FALSE;
				}
				memcpy (&sa, &ne->n_net, sizeof (sa));
/* ne->n_net in network order */
				in_addr.s_addr = sa.sin_addr.s_addr;
				if (IN_MULTICAST(in_addr.s_addr)) {
					pgm_set_error (error,
						     PGM_ERROR_DOMAIN_IF,
						     PGM_ERROR_XDEV,
						     _("Network name %s%s%s resolves to IPv4 mulicast address."),
						     ifname ? "\"" : "", ifname ? ifname : "(null)", ifname ? "\"" : "");
					if (NULL != error)
						pgm_debug ("parse_interface() failed: %s", (*error)->message);
					return FALSE;
				}
				check_inet_network = TRUE;
				check_addr = TRUE;
				break;
			}
			case AF_INET6: {
#ifdef HAVE_GETNETENT
				pgm_set_error (error,
					       PGM_ERROR_DOMAIN_IF,
					       PGM_ERROR_NODEV,
					       _("Not configured for IPv6 network name support, %s%s%s is an IPv6 network name."),
					       ifname ? "\"" : "", ifname ? ifname : "(null)", ifname ? "\"" : "");
				if (NULL != error)
					pgm_debug ("parse_interface() failed: %s", (*error)->message);
				return FALSE;
#else
				if (AF_INET == family) {
					pgm_set_error (error,
						     PGM_ERROR_DOMAIN_IF,
						     PGM_ERROR_NODEV,
						     _("IP address family conflict when resolving network name %s%s%s, found AF_INET6 when AF_INET expected."),
						     ifname ? "\"" : "", ifname ? ifname : "(null)", ifname ? "\"" : "");
					if (NULL != error)
						pgm_debug ("parse_interface() failed: %s", (*error)->message);
					return FALSE;
				}
				memcpy (&sa6_addr, &ne->n_net, sizeof (sa6_addr));
				if (IN6_IS_ADDR_MULTICAST(&sa6_addr.sin6_addr)) {
					pgm_set_error (error,
						     PGM_ERROR_DOMAIN_IF,
						     PGM_ERROR_XDEV,
						     _("Network name resolves to IPv6 mulicast address %s%s%s"),
						     ifname ? "\"" : "", ifname ? ifname : "(null)", ifname ? "\"" : "");
					if (NULL != error)
						pgm_debug ("parse_interface() failed: %s", (*error)->message);
					return FALSE;
				}
				check_inet6_network = TRUE;
				check_addr = TRUE;
				break;
#endif /* HAVE_GETNETENT */
			}
			default:
				pgm_set_error (error,
					     PGM_ERROR_DOMAIN_IF,
					     PGM_ERROR_NODEV,
					     _("Network name resolves to non-internet protocol address family %s%s%s"),
					     ifname ? "\"" : "", ifname ? ifname : "(null)", ifname ? "\"" : "");
				if (NULL != error)
					pgm_debug ("parse_interface() failed: %s", (*error)->message);
				return FALSE;
			}
		}
	}

/* hostname lookup with potential DNS delay or error */
	if (!check_addr)
	{
		char errbuf[1024];
		struct addrinfo hints = {
			.ai_family	= family,
			.ai_socktype	= SOCK_STREAM,		/* not really, SOCK_RAW */
			.ai_protocol	= IPPROTO_TCP,		/* not really, IPPROTO_PGM */
			.ai_flags	= AI_ADDRCONFIG,	/* AI_V4MAPPED is unhelpful */
		}, *result, *res;
		const int eai = getaddrinfo (ifname, NULL, &hints, &result);
		switch (eai) {
		case 0:
/* NB: getaddrinfo may return multiple addresses, one per interface & family.
 * The sorting order of the list defined by RFC 3484 and /etc/gai.conf.
 *
 * Ex.  hinano 127.0.1.1        // default Linux DHCP address due to lack of IPv4 link-local addressing
 *      hinano 10.6.15.88       // IPv4 address provided by DHCP
 *
 * Address 127.0.1.1 should be ignored as it is not multicast capable.
 */
			if (NULL != result->ai_next) /* more than one result */
			{
				addr_cnt = 0;
				for (res = result; NULL != res; res = res->ai_next)
				{
					if ((AF_INET == res->ai_family && IN_MULTICAST(pgm_ntohl (((struct sockaddr_in*)(res->ai_addr))->sin_addr.s_addr))) ||
					    (AF_INET6 == res->ai_family && IN6_IS_ADDR_MULTICAST(&((struct sockaddr_in6*)res->ai_addr)->sin6_addr)))
						continue;
					addr_cnt++;
				}
				pgm_debug ("getaddrinfo() returned %d addresses.", addr_cnt);
				if (addr_cnt > 1) /* copy all valid entries onto the stack */
				{
					unsigned i = 0;
					addr = pgm_newa (struct sockaddr_storage, addr_cnt);
					for (res = result; NULL != res; res = res->ai_next)
					{
						if ((AF_INET == res->ai_family && IN_MULTICAST(pgm_ntohl (((struct sockaddr_in*)(res->ai_addr))->sin_addr.s_addr))) ||
						    (AF_INET6 == res->ai_family && IN6_IS_ADDR_MULTICAST(&((struct sockaddr_in6*)res->ai_addr)->sin6_addr)))
							continue;
						memcpy (&addr[i++], res->ai_addr, pgm_sockaddr_len (res->ai_addr));
					}
					freeaddrinfo (result);
/* address list complete */
					check_addr = TRUE;
					break;
				}
				else if (1 == addr_cnt) /* find matching entry */
				{
					for (res = result; NULL != res; res = res->ai_next)
					{
						if ((AF_INET == res->ai_family && IN_MULTICAST(pgm_ntohl (((struct sockaddr_in*)(res->ai_addr))->sin_addr.s_addr))) ||
						    (AF_INET6 == res->ai_family && IN6_IS_ADDR_MULTICAST(&((struct sockaddr_in6*)res->ai_addr)->sin6_addr)))
							continue;
						break;
					}
/* verify entry was found */
					pgm_assert (NULL != res);
				}
				else /* addr_cnt == 0 ∴  use last entry */
				{
					for (res = result; NULL != res->ai_next; res = res->ai_next);
					addr_cnt++;
/* verify entry is valid */
					pgm_assert (NULL != res);
				}
			}
			else
			{
				pgm_debug ("getaddrinfo() returned 1 address.");
				res = result;	/* only one result */
			}

			if (AF_INET == res->ai_family &&
			    IN_MULTICAST(pgm_ntohl (((struct sockaddr_in*)(res->ai_addr))->sin_addr.s_addr)))
			{
				pgm_set_error (error,
					     PGM_ERROR_DOMAIN_IF,
					     PGM_ERROR_XDEV,
					     _("Expecting interface address, found IPv4 multicast name %s%s%s"),
					     ifname ? "\"" : "", ifname ? ifname : "(null)", ifname ? "\"" : "");
				freeaddrinfo (result);
				if (NULL != error)
					pgm_debug ("parse_interface() failed: %s", (*error)->message);
				return FALSE;
			}
			else if (AF_INET6 == res->ai_family &&
				 IN6_IS_ADDR_MULTICAST(&((struct sockaddr_in6*)res->ai_addr)->sin6_addr))
			{
				pgm_set_error (error,
					     PGM_ERROR_DOMAIN_IF,
					     PGM_ERROR_XDEV,
					     _("Expecting interface address, found IPv6 multicast name %s%s%s"),
					     ifname ? "\"" : "", ifname ? ifname : "(null)", ifname ? "\"" : "");
				freeaddrinfo (result);
				if (NULL != error)
					pgm_debug ("parse_interface() failed: %s", (*error)->message);
				return FALSE;
			}
			memcpy (addr, res->ai_addr, pgm_sockaddr_len (res->ai_addr));
			freeaddrinfo (result);
			check_addr = TRUE;
			break;

#if defined(EAI_NODATA) && EAI_NODATA != EAI_NONAME
		case EAI_NODATA:
#endif
		case EAI_NONAME:
			check_ifname = TRUE;
			break;

#ifndef _WIN32
/* Windows does not implement EAI_SYSTEM, everything is in the WSA domain.
 * Ubuntu 13.04 returns EAI_SYSTEM+ENOENT on IPv4 whilst IPv6 on a non-
 * capable network returns EAI_SYSTEM+ETIMEDOUT for dual-stack "wlan0".
 */
		case EAI_SYSTEM:
			if (ENOENT == errno || ETIMEDOUT == errno) {
				check_ifname = TRUE;
			} else {
				pgm_set_error (error,
					     PGM_ERROR_DOMAIN_IF,
					     pgm_error_from_eai_errno (eai, errno),
					     _("Internet host resolution system error: %s(%d)"),
					     pgm_strerror_s (errbuf, sizeof (errbuf), errno),
					     errno);
				if (NULL != error)
					pgm_debug ("parse_interface() failed: %s", (*error)->message);
				return FALSE;
			}
			break;
#endif

		default:
			pgm_set_error (error,
				     PGM_ERROR_DOMAIN_IF,
				     pgm_error_from_eai_errno (eai, errno),
				     _("Internet host resolution: %s(%d)"),
				     pgm_gai_strerror_s (errbuf, sizeof (errbuf), eai),
				     eai);
			if (NULL != error)
				pgm_debug ("parse_interface() failed: %s", (*error)->message);
			return FALSE;
		}
	}

/* iterate through interface list and match device name, ip or net address */
	if (!pgm_getifaddrs (&ifap, error)) {
		pgm_prefix_error (error,
				_("Enumerating network interfaces: "));
		if (NULL != error)
			pgm_debug ("parse_interface() failed: %s", (*error)->message);
		return FALSE;
	}

	for (ifa = ifap; ifa; ifa = ifa->ifa_next)
	{
		if (NULL == ifa->ifa_addr)
			continue;

		switch (ifa->ifa_addr->sa_family) {
/* ignore raw entries on Linux */
#ifdef AF_PACKET
		case AF_PACKET:
			continue;
#endif
		case AF_INET:
			if (AF_INET6 == family)
				continue;
			break;
		case AF_INET6:
			if (AF_INET == family)
				continue;
			break;
		default:
			continue;
		}

		const unsigned ifindex = pgm_if_nametoindex (ifa->ifa_addr->sa_family, ifa->ifa_name);
/* Some faulty systems may fail, handle this situation without raising an assertion. */
		const bool has_valid_if_name = (ifindex > 0);

/* check numeric host */
		if (check_addr)
		{
			for (unsigned i = 0; i < addr_cnt; i++)
			{
				if (0 == pgm_sockaddr_cmp (ifa->ifa_addr, (const struct sockaddr*)&addr[i]))
				{
					if (!has_valid_if_name)
						pgm_warn (_("Interface %s does not resolve to an interface index, multicast traffic will follow the systems routing table and may appear on a different network than specified."), ir->ir_name);
					pgm_strncpy_s (ir->ir_name, IF_NAMESIZE, ifa->ifa_name, _TRUNCATE);
					ir->ir_flags = ifa->ifa_flags;
					if (ir->ir_flags & IFF_LOOPBACK)
						pgm_warn (_("Interface %s reports as a loopback device."), ir->ir_name);
					if (!(ir->ir_flags & IFF_MULTICAST))
						pgm_warn (_("Interface %s reports as a non-multicast capable device."), ir->ir_name);
					ir->ir_interface = ifindex;
					memcpy (&ir->ir_addr, ifa->ifa_addr, pgm_sockaddr_len (ifa->ifa_addr));
					pgm_freeifaddrs (ifap);
#ifdef IF_DEBUG
					{
						char s[IR_STRLEN];
						pgm_debug ("parse_interface (\"%s\") evaluated as { %s }.",
							ifname, interface_req_to_string (ir, s, sizeof (s)));
					}
#endif
					return TRUE;
				}
			}
		}

/* check network address */
		if (check_inet_network &&
		    AF_INET == ifa->ifa_addr->sa_family)
		{
			const struct in_addr ifaddr  = { .s_addr = pgm_ntohl (((struct sockaddr_in*)ifa->ifa_addr)->sin_addr.s_addr) };
			const struct in_addr netmask = { .s_addr = pgm_ntohl (((struct sockaddr_in*)ifa->ifa_netmask)->sin_addr.s_addr) };
			struct in_addr lna;

/* local network address must be null, otherwise should match an address is previous check */
			if (!pgm_inet_lnaof (&lna, &in_addr, &netmask) &&
				is_in_net (&ifaddr, &in_addr, &netmask))
			{
				if (!has_valid_if_name) {
					pgm_warn (_("Skipping matching network device %s that fails reverse interface name lookup."), ir->ir_name);
					goto skip_inet_network;
				}
				pgm_strncpy_s (ir->ir_name, IF_NAMESIZE, ifa->ifa_name, _TRUNCATE);
				ir->ir_flags = ifa->ifa_flags;
				if (ir->ir_flags & IFF_LOOPBACK) {
					pgm_warn (_("Skipping matching loopback network device %s."), ir->ir_name);
					goto skip_inet_network;
				}
				if (!(ir->ir_flags & IFF_MULTICAST)) {
					pgm_warn (_("Skipping matching non-multicast capable network device %s."), ir->ir_name);
					goto skip_inet_network;
				}

/* check for multiple interfaces on same network */
				if (interface_matches++) {
					char saddr[INET_ADDRSTRLEN];
					pgm_set_error (error,
						     PGM_ERROR_DOMAIN_IF,
						     PGM_ERROR_NOTUNIQ,
						     _("Multiple interfaces found with network address %s."),
						     pgm_inet_ntop (AF_INET, &in_addr, saddr, sizeof(saddr)));
					pgm_freeifaddrs (ifap);
					if (NULL != error)
						pgm_debug ("parse_interface() failed: %s", (*error)->message);
					return FALSE;
				}

				ir->ir_interface = ifindex;
				memcpy (&ir->ir_addr, ifa->ifa_addr, pgm_sockaddr_len (ifa->ifa_addr));
				continue;
			}
		}
		if (check_inet6_network &&
		    AF_INET6 == ifa->ifa_addr->sa_family &&
/* no specified scope or matching scope */
		    (	0 == sa6_addr.sin6_scope_id ||
			((struct sockaddr_in6*)ifa->ifa_addr)->sin6_scope_id == sa6_addr.sin6_scope_id)	)
		{
			const struct in6_addr ifaddr = ((struct sockaddr_in6*)ifa->ifa_addr)->sin6_addr;
			const struct in6_addr netmask = ((struct sockaddr_in6*)ifa->ifa_netmask)->sin6_addr;
			struct in6_addr lna;

			if (!pgm_inet6_lnaof (&lna, &sa6_addr.sin6_addr, &netmask) &&
				is_in_net6 (&ifaddr, &sa6_addr.sin6_addr, &netmask))
			{
				if (!has_valid_if_name) {
					pgm_warn (_("Skipping matching network device %s that fails reverse interface name lookup."), ir->ir_name);
					goto skip_inet_network;
				}
				pgm_strncpy_s (ir->ir_name, IF_NAMESIZE, ifa->ifa_name, _TRUNCATE);
				ir->ir_flags = ifa->ifa_flags;
				if (ir->ir_flags & IFF_LOOPBACK) {
					pgm_warn (_("Skipping matching loopback network device %s."), ir->ir_name);
					goto skip_inet_network;
				}
				if (!(ir->ir_flags & IFF_MULTICAST)) {
					pgm_warn (_("Skipping matching non-multicast capable network device %s."), ir->ir_name);
					goto skip_inet_network;
				}

/* check for multiple interfaces on same network */
				if (interface_matches++) {
					char saddr[INET6_ADDRSTRLEN];
					pgm_sockaddr_ntop ((struct sockaddr*)&sa6_addr, saddr, sizeof (saddr));
					pgm_set_error (error,
						       PGM_ERROR_DOMAIN_IF,
						       PGM_ERROR_NOTUNIQ,
						       _("Multiple interfaces found with network address %s."),
						       saddr);
					pgm_freeifaddrs (ifap);
					if (NULL != error)
						pgm_debug ("parse_interface() failed: %s", (*error)->message);
					return FALSE;
				}

				ir->ir_interface = ifindex;
				memcpy (&ir->ir_addr, ifa->ifa_addr, pgm_sockaddr_len (ifa->ifa_addr));
				continue;
			}
		}
skip_inet_network:

/* check interface name */
		if (check_ifname)
		{
			if (0 != strcmp (ifname, ifa->ifa_name))
				continue;

/* skip devices that fail reverse name lookup */
			if (!has_valid_if_name)
				continue;
			ir->ir_flags = ifa->ifa_flags;
/* skip loopback and non-multicast capable devices */
			if ((ir->ir_flags & IFF_LOOPBACK) || !(ir->ir_flags & IFF_MULTICAST))
				continue;

			interface_matches++;
			ir->ir_interface = ifindex;
			pgm_strncpy_s (ir->ir_name, IF_NAMESIZE, ifa->ifa_name, _TRUNCATE);
			if (1 == interface_matches) {
				memcpy (&ir->ir_addr, ifa->ifa_addr, pgm_sockaddr_len (ifa->ifa_addr));
			} else {
/* one of many addresses, leave address undetermined. */
				memset (&ir->ir_addr, 0, sizeof (struct sockaddr_storage));
			}
			continue;
		}
	}

	if (0 == interface_matches) {
		pgm_set_error (error,
			     PGM_ERROR_DOMAIN_IF,
			     PGM_ERROR_NODEV,
			     _("No matching non-loopback and multicast capable network interface %s%s%s"),
			     ifname ? "\"" : "", ifname ? ifname : "(null)", ifname ? "\"" : "");
		pgm_freeifaddrs (ifap);
		if (NULL != error)
			pgm_debug ("parse_interface() failed: %s", (*error)->message);
		return FALSE;
	}

#ifdef IF_DEBUG
	{
		char s[IR_STRLEN];
		pgm_debug ("parse_interface (\"%s\") evaluted as { %s }.",
			ifname, interface_req_to_string (ir, s, sizeof (s)));
	}
#endif
	pgm_freeifaddrs (ifap);
	return TRUE;
}

/* parse one multicast address, conflict resolution of multiple address families of DNS multicast names is
 * deferred to libc.
 *
 * Zone indices are ignored as interface specification is already available.
 *
 * reserved addresses may flag warnings:
 *
 * 224.0.0.0/24 for local network control
 * 224.0.1/24 for internetwork control
 * 169.254.255.255, ff02::1 all local nodes on segment
 * ff02::2 all routers
 * ff05::1 all nodes
 * ff0x::fb multicast DNS
 * ff0x::108 NIS
 * ff05::1:3 DHCP
 *
 * returns TRUE on success, FALSE on error and sets error appropriately.
 */

static
bool
parse_group (
	const int		  family,	/* AF_UNSPEC | AF_INET | AF_INET6 */
	const char*	 restrict group,	/* NULL terminated */
	struct sockaddr* restrict addr,		/* pointer to sockaddr_storage for writing */
	pgm_error_t**	 restrict error
	)
{
/* pre-conditions */
	pgm_assert (AF_INET == family || AF_INET6 == family || AF_UNSPEC == family);
	pgm_assert (NULL != group);
	pgm_assert (NULL != addr);

	pgm_debug ("parse_group (family:%s group:%s%s%s addr:%p error:%p)",
		pgm_family_string (family),
		group ? "\"" : "", group ? group : "(null)", group ? "\"" : "",
		(const void*)addr,
		(const void*)error);

/* strip any square brackets for early IPv6 literal evaluation */
	if (AF_INET != family &&
	    '[' == group[0])
	{
		const size_t grouplen = strlen(group);
		if (']' == group[ grouplen - 1 ]) {
			char literal[1024];
			pgm_strncpy_s (literal, sizeof (literal), group + 1, grouplen - 2);
			if (pgm_inet_pton (AF_INET6, literal, &((struct sockaddr_in6*)addr)->sin6_addr) &&
			    IN6_IS_ADDR_MULTICAST(&((struct sockaddr_in6*)addr)->sin6_addr))
			{
				addr->sa_family = AF_INET6;
				((struct sockaddr_in6*)addr)->sin6_port = 0;
				((struct sockaddr_in6*)addr)->sin6_flowinfo = 0;
				((struct sockaddr_in6*)addr)->sin6_scope_id = 0;
#ifdef IF_DEBUG
				{
					char multicast_group[1024];
					if (0 != pgm_sockaddr_ntop (addr, multicast_group, sizeof (multicast_group)))
						multicast_group[0] = 0;
					pgm_debug ("parse_group() evaluated as \"%s\".", multicast_group);
				}
#endif
				return TRUE;
			}
		}
	}

/* IPv4 address */
	if (AF_INET6 != family &&
	    pgm_inet_pton (AF_INET, group, &((struct sockaddr_in*)addr)->sin_addr) &&
	    IN_MULTICAST(pgm_ntohl (((struct sockaddr_in*)addr)->sin_addr.s_addr)))
	{
		addr->sa_family = AF_INET;
#ifdef IF_DEBUG
		{
			char multicast_group[1024];
			if (0 != pgm_sockaddr_ntop (addr, multicast_group, sizeof (multicast_group)))
				multicast_group[0] = 0;
			pgm_debug ("parse_group() evaluated as \"%s\".", multicast_group);
		}
#endif
		return TRUE;
	}
	if (AF_INET  != family &&
	    pgm_inet_pton (AF_INET6, group, &((struct sockaddr_in6*)addr)->sin6_addr) &&
	    IN6_IS_ADDR_MULTICAST(&((struct sockaddr_in6*)addr)->sin6_addr))
	{
		addr->sa_family = AF_INET6;
		((struct sockaddr_in6*)addr)->sin6_port = 0;
		((struct sockaddr_in6*)addr)->sin6_flowinfo = 0;
		((struct sockaddr_in6*)addr)->sin6_scope_id = 0;
#ifdef IF_DEBUG
		{
			char multicast_group[1024];
			if (0 != pgm_sockaddr_ntop (addr, multicast_group, sizeof (multicast_group)))
				multicast_group[0] = 0;
			pgm_debug ("parse_group() evaluated as \"%s\".", multicast_group);
		}
#endif
		return TRUE;
	}

/* NSS network */
	const struct pgm_netent_t* ne = pgm_getnetbyname (group);
/* ne::n_net in host byte order */
	if (ne) {
		switch (ne->n_net.ss_family) {
		case AF_INET: {
			struct sockaddr_in sa;
			if (AF_INET6 == family) {
				pgm_set_error (error,
					     PGM_ERROR_DOMAIN_IF,
					     PGM_ERROR_NODEV,
					     _("IP address family conflict when resolving network name %s%s%s, found IPv4 when IPv6 expected."),
					     group ? "\"" : "", group ? group : "(null)", group ? "\"" : "");
				if (NULL != error)
					pgm_debug ("parse_group() failed: %s", (*error)->message);
				return FALSE;
			}
			memcpy (&sa, &ne->n_net, sizeof (sa));
			if (IN_MULTICAST(sa.sin_addr.s_addr)) {
				addr->sa_family = ne->n_net.ss_family;
				((struct sockaddr_in*)addr)->sin_addr.s_addr = pgm_htonl (sa.sin_addr.s_addr);
#ifdef IF_DEBUG
				{
					char multicast_group[1024];
					if (0 != pgm_sockaddr_ntop (addr, multicast_group, sizeof (multicast_group)))
						multicast_group[0] = 0;
					pgm_debug ("parse_group() evaluated as \"%s\".", multicast_group);
				}
#endif
				return TRUE;
			}
			pgm_set_error (error,
				     PGM_ERROR_DOMAIN_IF,
				     PGM_ERROR_NODEV,
				     _("IP address class conflict when resolving network name %s%s%s, expected IPv4 multicast."),
				     group ? "\"" : "", group ? group : "(null)", group ? "\"" : "");
			if (NULL != error)
				pgm_debug ("parse_group() failed: %s", (*error)->message);
			return FALSE;
		}
		case AF_INET6: {
#ifdef HAVE_GETNETENT
			pgm_set_error (error,
				     PGM_ERROR_DOMAIN_IF,
				     PGM_ERROR_NODEV,
				     _("Not configured for IPv6 network name support, %s%s%s is an IPv6 network name."),
				     group ? "\"" : "", group ? group : "(null)", group ? "\"" : "");
			if (NULL != error)
				pgm_debug ("parse_group() failed: %s", (*error)->message);
			return FALSE;
#else
			struct sockaddr_in6 sa6;
			if (AF_INET == family) {
				pgm_set_error (error,
					     PGM_ERROR_DOMAIN_IF,
					     PGM_ERROR_NODEV,
					     _("IP address family conflict when resolving network name %s%s%s, found IPv6 when IPv4 expected."),
					     group ? "\"" : "", group ? group : "(null)", group ? "\"" : "");
				if (NULL != error)
					pgm_debug ("parse_group() failed: %s", (*error)->message);
				return FALSE;
			}
			memcpy (&sa6, &ne->n_net, sizeof (sa6));
			if (IN6_IS_ADDR_MULTICAST(&sa6.sin6_addr)) {
				memcpy (addr, &sa6, sizeof (sa6));
#ifdef IF_DEBUG
				{
					char multicast_group[1024];
					if (0 != pgm_sockaddr_ntop (addr, multicast_group, sizeof (multicast_group)))
						multicast_group[0] = 0;
					pgm_debug ("parse_group() evaluated as \"%s\".", multicast_group);
				}
#endif
				return TRUE;
			}
			pgm_set_error (error,
				     PGM_ERROR_DOMAIN_IF,
				     PGM_ERROR_NODEV,
				     _("IP address class conflict when resolving network name %s%s%s, expected IPv6 multicast."),
				     group ? "\"" : "", group ? group : "(null)", group ? "\"" : "");
			if (NULL != error)
				pgm_debug ("parse_group() failed: %s", (*error)->message);
			return FALSE;
#endif /* HAVE_GETNETENT */
		}
		default:
			pgm_set_error (error,
				     PGM_ERROR_DOMAIN_IF,
				     PGM_ERROR_NODEV,
				     _("Network name resolves to non-internet protocol address family %s%s%s"),
				     group ? "\"" : "", group ? group : "(null)", group ? "\"" : "");
			if (NULL != error)
				pgm_debug ("parse_group() failed: %s", (*error)->message);
			return FALSE;
		}
	}

/* lookup group through name service */
	struct addrinfo hints = {
		.ai_family	= family,
		.ai_socktype	= SOCK_STREAM,		/* not really, SOCK_RAW */
		.ai_protocol	= IPPROTO_TCP,		/* not really, IPPROTO_PGM */
		.ai_flags	= AI_ADDRCONFIG,	/* AI_V4MAPPED is unhelpful */
	}, *result, *res;

	const int eai = getaddrinfo (group, NULL, &hints, &result);
	if (0 != eai) {
		char errbuf[1024];
		pgm_set_error (error,
			     PGM_ERROR_DOMAIN_IF,
			     pgm_error_from_eai_errno (eai, errno),
			     _("Resolving receive group: %s(%d)"),
			     pgm_gai_strerror_s (errbuf, sizeof (errbuf), eai),
			     eai);
		if (NULL != error)
			pgm_debug ("parse_group() failed: %s", (*error)->message);
		return FALSE;
	}

/* NB: getaddrinfo may return multiple addresses, one per interface & family, only the first
 * return result is used.  The sorting order of the list defined by RFC 3484 and /etc/gai.conf
 */
	for (res = result; NULL != res; res = res->ai_next)
	{
		if ((AF_INET6 != family && IN_MULTICAST(pgm_ntohl (((const struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr))) ||
		    (AF_INET  != family && IN6_IS_ADDR_MULTICAST(&((struct sockaddr_in6*)res->ai_addr)->sin6_addr)))
		{
/* return first multicast result */
			memcpy (addr, res->ai_addr, res->ai_addrlen);
			freeaddrinfo (result);
#ifdef IF_DEBUG
			{
				char multicast_group[1024];
				if (0 != pgm_sockaddr_ntop (addr, multicast_group, sizeof (multicast_group)))
					multicast_group[0] = 0;
				pgm_debug ("parse_group() evaluated as \"%s\".", multicast_group);
			}
#endif
			return TRUE;
		}
	}

	pgm_set_error (error,
		     PGM_ERROR_DOMAIN_IF,
		     PGM_ERROR_INVAL,
		     _("Unresolvable receive group %s%s%s"),
		     group ? "\"" : "", group ? group : "(null)", group ? "\"" : "");
	freeaddrinfo (result);
	if (NULL != error)
		pgm_debug ("parse_group() failed: %s", (*error)->message);
	return FALSE;
}

/* parse an interface entity from a network parameter.
 *
 * family can be unspecified - AF_UNSPEC, can return interfaces with the unspecified
 * address family
 *
 * examples:  "eth0"
 * 	      "hme0,hme1"
 * 	      "qe0,qe1,qe2"
 * 	      "qe0,qe2,qe2"	=> valid even though duplicate interface name
 *
 * returns TRUE on success with device_list containing double linked list of devices as
 * sockaddr/idx pairs.  returns FALSE on error, including multiple matching adapters.
 *
 * memory ownership of linked list is passed to caller and must be freed with pgm_free
 * and the pgm_list_free* api.
 */

static
bool
parse_interface_entity (
	int			family,	/* AF_UNSPEC | AF_INET | AF_INET6 */
	const char*   restrict	entity,	/* NULL terminated */
	pgm_list_t**  restrict	interface_list,	/* <struct interface_req*> */
	pgm_error_t** restrict	error
	)
{
	struct interface_req* ir;
	pgm_list_t* source_list = NULL;

/* pre-conditions */
	pgm_assert (AF_INET == family || AF_INET6 == family || AF_UNSPEC == family);
	pgm_assert (NULL != interface_list);
	pgm_assert (NULL == *interface_list);

	pgm_debug ("parse_interface_entity (family:%s entity:%s%s%s interface_list:%p error:%p)",
		pgm_family_string (family),
		entity ? "\"":"", entity ? entity : "(null)", entity ? "\"":"",
		(const void*)interface_list,
		(const void*)error);

/* the empty entity, returns in_addr_any for both receive and send interfaces */
	if (NULL == entity)
	{
		ir = pgm_new0 (struct interface_req, 1);
		ir->ir_addr.ss_family = family;
		*interface_list = pgm_list_append (*interface_list, ir);
		return TRUE;
	}

/* check interface name length limit */
	char** tokens = pgm_strsplit (entity, ",", 10);
	int j = 0;
	while (tokens && tokens[j])
	{
		pgm_error_t* sub_error = NULL;
		ir = pgm_new (struct interface_req, 1);
		if (!parse_interface (family, tokens[j], ir, &sub_error))
		{
/* mark multiple interfaces for later decision based on group families */
			if (sub_error && PGM_ERROR_NOTUNIQ == sub_error->code)
			{
				ir->ir_addr.ss_family = AF_UNSPEC;
				pgm_error_free (sub_error);
			}
/* bail out on first interface with an error */
			else
			{
				pgm_propagate_error (error, sub_error);
				pgm_free (ir);
				pgm_strfreev (tokens);
				while (source_list) {
					pgm_free (source_list->data);
					source_list = pgm_list_delete_link (source_list, source_list);
				}
				return FALSE;
			}
		}

		source_list = pgm_list_append (source_list, ir);
		++j;
	}

	pgm_strfreev (tokens);
	*interface_list = source_list;
	return TRUE;
}

/* With type-punning safe code copy the default multicast group into the provided
 * sockaddr pointer.  The compiler will clean up the redundancy.
 */
static
bool
set_default_multicast_group (
	int family,
	struct sockaddr* sa
	)
{
	switch (family) {
	case AF_INET6: {
		struct sockaddr_in6 s6;
		memset (&s6, 0, sizeof (s6));
		memcpy (&s6.sin6_addr, &if6_default_group_addr, sizeof (if6_default_group_addr));
		memcpy (sa, &s6, sizeof (s6));
		break;
	}

	case AF_INET: {
		struct sockaddr_in s4;
		memset (&s4, 0, sizeof (s4));
		s4.sin_addr.s_addr = pgm_htonl (IF_DEFAULT_GROUP);
		memcpy (sa, &s4, sizeof (s4));
		break;
	}

	default:
		return FALSE;
	}

	sa->sa_family = family;
	return TRUE;
}

/* Given an entity parameter, attempt to resolve into a list of multicast groups.  Resolution
 * may fail without knowing a preferred address family.
 */

static
bool
resolve_af_from_entity (
	const char* restrict entity,
	int* address_family
	)
{
	struct sockaddr_storage ss;
	int j = 0;	
	char** tokens = pgm_strsplit (entity ? entity : "", ",", 10);
	while (tokens && tokens[j])
	{
		if (parse_group (AF_UNSPEC, tokens[j], (struct sockaddr*)&ss, NULL /* ignore error */))
		{
			pgm_strfreev (tokens);
			*address_family = ss.ss_family;
			return TRUE;
		}
		++j;
	}

	pgm_strfreev (tokens);
	return FALSE;
}

/* Resolve an entity parameter into a list of gsr entries by the specified
 * address family and interface.
 */

static
bool
resolve_gsr_from_entity (
	int			address_family,
	const char* restrict	entity,
	pgm_list_t**  restrict	gsr_list,	/* <struct pgm_group_source_req*> */
	pgm_error_t** restrict	error
	)
{
	struct pgm_group_source_req* gsr;
	pgm_list_t* list = NULL;
	int j = 0;
	char** tokens = pgm_strsplit (entity ? entity : "", ",", 10);
	while (tokens && tokens[j])
	{
		gsr = pgm_new0 (struct pgm_group_source_req, 1);

		if (!parse_group (address_family, tokens[j], (struct sockaddr*)&gsr->gsr_group, error))
		{
			pgm_free (gsr);
			while (list) {
				pgm_free (list->data);
				list = pgm_list_delete_link (list, list);
			}
			pgm_strfreev (tokens);
			if (NULL != error)
				pgm_debug ("resolve_gsr_from_entity() failed: %s", (*error)->message);
			return FALSE;
		}
		else
		{
/* ASM: source = group */
			memcpy (&gsr->gsr_source, &gsr->gsr_group, pgm_sockaddr_len ((struct sockaddr*)&gsr->gsr_group));
			list = pgm_list_append (list, gsr);
		}
		++j;
	}

	pgm_strfreev (tokens);
	*gsr_list = list;
	return TRUE;
}

/* bind every multicast group to a given interface.
 */

static
bool
bind_gsr_to_interface (
	pgm_list_t*  restrict  gsr_list,	/* <struct pgm_group_source_req*> */
	const struct interface_req* ir
	)
{
	while (gsr_list) {
		struct pgm_group_source_req* gsr = gsr_list->data;
/* interface is a tuple of {index, address} in order to support modern IP stacks with IP aliasing. */
		gsr->gsr_interface = ir->ir_interface;
		memcpy (&gsr->gsr_addr, &ir->ir_addr, pgm_sockaddr_len ((const struct sockaddr*)&ir->ir_addr));
		gsr_list = gsr_list->next;
	}
	return TRUE;
}

/* given an interface resolve the intended address family.
 */

static
bool
resolve_af_from_interface (
	const struct interface_req* ir,
	int* address_family
	)
{
	if (AF_UNSPEC == ir->ir_addr.ss_family)
	{
		return FALSE;
	}
	else
	{
		*address_family = ir->ir_addr.ss_family;
		return TRUE;
	}
}

/* determine preferred address family from the primary node address, likely to be IPv6 unless disabled.
 */

static
bool
resolve_af_from_node (
	int* address_family,
	pgm_error_t** restrict	error
	)
{
	struct sockaddr_storage addr;
	if (!pgm_get_multicast_enabled_node_addr (AF_UNSPEC, (struct sockaddr*)&addr, sizeof(addr), error))
	{
		pgm_prefix_error (error,
				_("Node primary node address family cannot be determined: "));
		if (NULL != error)
			pgm_debug ("resolve_interface() failed: %s", (*error)->message);
		return FALSE;
	}
	else
	{
		pgm_debug ("Node primary node address family detected as %s.", pgm_family_string (addr.ss_family));
		*address_family = addr.ss_family;
		return TRUE;
	}
}

/* resolve a non-unique interface decl using an address family, an empty decl 
 * defaults to the node primary multicast-capable address.
 */

static
bool
resolve_interface (
	const int		address_family,
	struct interface_req*	ir,
	pgm_error_t** restrict	error
	)
{
	if (0 == ir->ir_name[0])
	{
		pgm_debug ("Interface is not specified, defaulting to node address.");
		struct sockaddr_storage addr;
		if (!pgm_get_multicast_enabled_node_addr (address_family, (struct sockaddr*)&addr, sizeof(addr), error))
		{
			pgm_prefix_error (error,
					_("Node primary node address cannot be determined: "));
			if (NULL != error)
				pgm_debug ("resolve_interface() failed: %s", (*error)->message);
		}
		else
		{
			{
				char s[INET6_ADDRSTRLEN];
				if (0 != pgm_sockaddr_ntop ((struct sockaddr*)&addr, s, sizeof(s)))
					s[0] = 0;
				pgm_debug ("Node primary node address detected as \"%s\".", s);
			}
			ir->ir_interface = pgm_sockaddr_scope_id ((struct sockaddr*)&addr);
			memcpy (&ir->ir_addr, &addr, pgm_sockaddr_len ((struct sockaddr*)&addr));
			return TRUE;
		}
	}
	else
	{
		pgm_debug ("Resolving interface \"%s\" with node address multicast group family %s.",
			ir->ir_name,
			pgm_family_string (address_family));
		struct interface_req resolved_interface;
		if (!parse_interface (address_family, ir->ir_name, &resolved_interface, error))
		{
			pgm_prefix_error (error,
					_("Unique address cannot be determined for interface %s%s%s: "),
					ir->ir_name ? "\"" : "", ir->ir_name ? ir->ir_name : "(null)", ir->ir_name ? "\"" : "");
			if (NULL != error)
				pgm_debug ("resolve_interface() failed: %s", (*error)->message);
		}
		else
		{
			{
				char s[INET6_ADDRSTRLEN];
				if (0 != pgm_sockaddr_ntop ((struct sockaddr*)&resolved_interface.ir_addr, s, sizeof(s)))
					s[0] = 0;
				pgm_debug ("Interface \"%s\" detected as \"%s\".", ir->ir_name ? ir->ir_name : "", s);
			}
			ir->ir_interface = resolved_interface.ir_interface;
			memcpy (&ir->ir_addr, &resolved_interface.ir_addr, pgm_sockaddr_len ((struct sockaddr*)&resolved_interface.ir_addr));
			return TRUE;
		}
	}

	return FALSE;
}

/* parse a receive multicast group entity.  can contain more than one multicast group to
 * support asymmetric fan-out.
 *
 * if group is ambiguous, i.e. empty or a name mapping then the address family of the matching
 * interface is queried.  if the interface is also ambiguous, i.e. empty interface and receive group
 * then the hostname will be used to determine the default node address family.  if the hosts
 * node name resolves both IPv4 and IPv6 address families then the first matching value is taken.
 *
 * e.g. "239.192.0.1"
 * 	"239.192.0.100,239.192.0.101"
 *
 * unspecified address family interfaces are forced to AF_INET or AF_INET6.
 *
 * returns TRUE on success, returns FALSE on error and sets error appropriately.
 */

static
bool
parse_receive_entity (
	int			family,		/* AF_UNSPEC | AF_INET | AF_INET6 */
	const char*   restrict	entity,		/* NULL terminated */
	pgm_list_t**  restrict	interface_list,	/* <struct interface_req*> */
	pgm_list_t**  restrict	recv_list,	/* <struct pgm_group_source_req*> */
	pgm_error_t** restrict	error
	)
{
/* pre-conditions */
	pgm_assert (AF_INET == family || AF_INET6 == family || AF_UNSPEC == family);
	pgm_assert (NULL != recv_list);
	pgm_assert (NULL == *recv_list);

	pgm_debug ("parse_receive_entity (family:%s entity:%s%s%s interface_list:%p recv_list:%p error:%p)",
		pgm_family_string (family),
		entity ? "\"":"", entity ? entity : "(null)", entity ? "\"":"",
		(const void*)interface_list,
		(const void*)recv_list,
		(const void*)error);

/* resolve address family from given parameters. */
	if (AF_UNSPEC == family) {
		if (!resolve_af_from_entity (entity, &family)) {
			pgm_trace (PGM_LOG_ROLE_CONFIGURATION, "Cannot resolve address family from entity parameter \"%s\".", entity ? entity : "");
		} else {
			pgm_trace (PGM_LOG_ROLE_CONFIGURATION, "Resolved entity parameter \"%s\" as %s.",
				entity ? entity : "",
				pgm_family_string (family));
		}
	}
	if (AF_UNSPEC == family) {
		if (!resolve_af_from_interface ((*interface_list)->data, &family)) {
			char s[IR_STRLEN];
			pgm_trace (PGM_LOG_ROLE_CONFIGURATION, "Cannot resolve address family from interface parameter { %s }.",
				interface_req_to_string ((*interface_list)->data, s, sizeof (s)));
		} else {
			char s[IR_STRLEN];
			pgm_trace (PGM_LOG_ROLE_CONFIGURATION, "Resolved interface parameter as { %s }.",
				interface_req_to_string ((*interface_list)->data, s, sizeof (s)));
		}
	}
/* final attempt defaulting to the primary node address. */
	if (AF_UNSPEC == family) {
		if (!resolve_af_from_node (&family, error)) {
			pgm_prefix_error (error,
					_("Node primary address family cannot be determined: "));
			return FALSE;
		} else {
			pgm_trace (PGM_LOG_ROLE_CONFIGURATION, "Resolved primary node as %s.",
				pgm_family_string (family));
		}
	}

/* if the interface in the network parameter is non-unique */
	if (AF_UNSPEC == ((struct interface_req*)(*interface_list)->data)->ir_addr.ss_family) {
		if (!resolve_interface (family, (struct interface_req*)(*interface_list)->data, error)) {
			pgm_prefix_error (error,
					_("Cannot resolve interface: "));
			return FALSE;
		} else {
			char s[IR_STRLEN];
			pgm_trace (PGM_LOG_ROLE_CONFIGURATION, "Resolved interface as { %s }.",
				interface_req_to_string ((struct interface_req*)(*interface_list)->data, s, sizeof (s)));
		}
	}

	if (NULL == entity) {
		struct pgm_group_source_req* gsr = pgm_new0 (struct pgm_group_source_req, 1);
		gsr->gsr_interface = ((struct interface_req*)(*interface_list)->data)->ir_interface;
		if (!set_default_multicast_group (family, (struct sockaddr*)&gsr->gsr_group)) {
			pgm_set_error (error,
					PGM_ERROR_DOMAIN_IF,
					PGM_ERROR_INVAL,
		     			_("Invalid address family %d"),
					family);
			pgm_free (gsr);
			return FALSE;
		} else {
			char s[INET6_ADDRSTRLEN];
			if (0 != pgm_sockaddr_ntop ((struct sockaddr*)&gsr->gsr_group, s, sizeof (s)))
				s[0] = 0;
			pgm_trace (PGM_LOG_ROLE_CONFIGURATION, "Setting default multicast group \"%s\".", s);
		}
/* ASM: source = group */
		memcpy (&gsr->gsr_source, &gsr->gsr_group, pgm_sockaddr_len ((struct sockaddr*)&gsr->gsr_group));
		*recv_list = pgm_list_append (*recv_list, gsr);
	} else {
		if (!resolve_gsr_from_entity (family, entity, recv_list, error)) {
			pgm_prefix_error (error,
					_("Cannot resolve entity: "));
			return FALSE;
		} else {
			char s[1024];
			s[0] = 0;
			for (pgm_list_t* gsr_list = *recv_list; gsr_list; gsr_list = gsr_list->next) {
				char t[1024];
				if (gsr_list == *recv_list) strcat (s, "{ ");
				else strcat (s, ", { ");
				pgm_gsr_to_string (gsr_list->data, t, sizeof (t));
				strcat (s, t);
				strcat (s, " }");
			}
			pgm_trace (PGM_LOG_ROLE_CONFIGURATION, "Resolved entity \"%s\" to [%s].", entity, s);
		}
	}

	if (!bind_gsr_to_interface (*recv_list, (struct interface_req*)(*interface_list)->data))
	{
		while (*recv_list) {
			pgm_free ((*recv_list)->data);
			*recv_list = pgm_list_delete_link (*recv_list, *recv_list);
		}
		return FALSE;
	}

#ifdef IF_DEBUG
	{
		char s[1024];
		s[0] = 0;
		for (pgm_list_t* gsr_list = *recv_list; gsr_list; gsr_list = gsr_list->next) {
			char t[1024];
			if (gsr_list == *recv_list) strcat (s, "{ ");
			else strcat (s, ", { ");
			pgm_gsr_to_string (gsr_list->data, t, sizeof (t));
			strcat (s, t);
			strcat (s, " }");
		}
		pgm_debug ("parse_receive_entity() evaluated as [%s].", s);
	}
#endif
	return TRUE;
}

static
bool
parse_send_entity (
	int			family,		/* AF_UNSPEC | AF_INET | AF_INET6 */
	const char*   restrict	entity,		/* null = empty entity */
	pgm_list_t**  restrict	interface_list,	/* <struct interface_req*> */
	pgm_list_t**  restrict	recv_list,	/* <struct pgm_group_source_req*> */
	pgm_list_t**  restrict	send_list,	/* <struct pgm_group_source_req*> */
	pgm_error_t** restrict	error
	)
{
/* pre-conditions */
	pgm_assert (AF_INET == family || AF_INET6 == family || AF_UNSPEC == family);
	pgm_assert (NULL != recv_list);
	pgm_assert (NULL != *recv_list);
	pgm_assert (NULL != send_list);
	pgm_assert (NULL == *send_list);

	pgm_debug ("parse_send_entity (family:%s entity:%s%s%s interface_list:%p recv_list:%p send_list:%p error:%p)",
		pgm_family_string (family),
		entity ? "\"":"", entity ? entity : "(null)", entity ? "\"":"",
		(const void*)interface_list,
		(const void*)recv_list,
		(const void*)send_list,
		(const void*)error);

	struct pgm_group_source_req* send_gsr;
	const struct interface_req* primary_interface = (struct interface_req*)(*interface_list)->data;

	if (entity == NULL)
	{
		send_gsr = pgm_memdup ((*recv_list)->data, sizeof(struct pgm_group_source_req));
		*send_list = pgm_list_append (*send_list, send_gsr);
#ifdef IF_DEBUG
		{
			char s[1024];
			s[0] = 0;
			for (pgm_list_t* gsr_list = *send_list; gsr_list; gsr_list = gsr_list->next) {
				char t[1024];
				if (gsr_list == *send_list) strcat (s, "{ ");
				else strcat (s, ", { ");
				pgm_gsr_to_string (gsr_list->data, t, sizeof (t));
				strcat (s, t);
				strcat (s, " }");
			}
			pgm_debug ("parse_send_entity() evaluated as [%s].", s);
		}
#endif
		return TRUE;
	}

/* default send object */
	send_gsr = pgm_new0 (struct pgm_group_source_req, 1);
	send_gsr->gsr_interface = primary_interface->ir_interface;
	memcpy (&send_gsr->gsr_addr, &primary_interface->ir_addr, pgm_sockaddr_len ((const struct sockaddr*)&primary_interface->ir_addr));
	if (!parse_group (family, entity, (struct sockaddr*)&send_gsr->gsr_group, error))
	{
		pgm_prefix_error (error,
				_("Unresolvable send entity %s%s%s: "),
				entity ? "\"":"", entity ? entity : "(null)", entity ? "\"":"");
		pgm_free (send_gsr);
		if (NULL != error)
			pgm_debug ("parse_send_entity() failed: %s", (*error)->message);
		return FALSE;
	}

/* check if we can now resolve the source interface by address family of the send group */
	if (AF_UNSPEC == primary_interface->ir_addr.ss_family)
	{
		if (primary_interface->ir_name[0] != '\0')
		{
			struct interface_req ir;
			if (!parse_interface (send_gsr->gsr_group.ss_family, primary_interface->ir_name, &ir, error))
			{
				pgm_prefix_error (error,
						_("Unique address cannot be determined for interface %s%s%s: "),
						primary_interface->ir_name ? "\"":"", primary_interface->ir_name ? primary_interface->ir_name : "(null)", primary_interface->ir_name ? "\"":"");
				pgm_free (send_gsr);
				if (NULL != error)
					pgm_debug ("parse_send_entity() failed: %s", (*error)->message);
				return FALSE;
			}

			send_gsr->gsr_interface = ir.ir_interface;
			((struct sockaddr_in6*)&send_gsr->gsr_group)->sin6_scope_id = pgm_sockaddr_scope_id ((struct sockaddr*)&ir.ir_addr);
			memcpy (&send_gsr->gsr_addr, &ir.ir_addr, pgm_sockaddr_len ((struct sockaddr*)&ir.ir_addr));
		}
	}

/* ASM: source = group */
	memcpy (&send_gsr->gsr_source, &send_gsr->gsr_group, pgm_sockaddr_len ((struct sockaddr*)&send_gsr->gsr_group));

/* scoped multicast groups are invalid on Windows */
#ifdef _WIN32
	if (AF_INET6 == send_gsr->gsr_group.ss_family) {
		((struct sockaddr_in6*)&send_gsr->gsr_group)->sin6_scope_id = 0;
	}
#endif

	*send_list = pgm_list_append (*send_list, send_gsr);
#ifdef IF_DEBUG
	{
		char s[1024];
		s[0] = 0;
		for (pgm_list_t* gsr_list = *send_list; gsr_list; gsr_list = gsr_list->next) {
			char t[1024];
			if (gsr_list == *send_list) strcat (s, "{ ");
			else strcat (s, ", { ");
			pgm_gsr_to_string (gsr_list->data, t, sizeof (t));
			strcat (s, t);
			strcat (s, " }");
		}
		pgm_debug ("parse_send_entity() evaluated as [%s].", s);
	}
#endif
	return TRUE;
}

/* parse network parameter
 *
 * interface list; receive multicast group list; send multicast group
 */

#define IS_HOSTNAME(x) ( 				/* RFC 952 */ \
				isalnum(x) || \
				((x) == '-') || \
				((x) == '.') \
			)
#define IS_IP(x) ( \
				isdigit(x) || \
				((x) == '.') || \
				((x) == '/') \
			)
#define IS_IP6(x) ( \
				isxdigit(x) || \
				((x) == ':') || \
				((x) == '/') || \
				((x) == '.') || \
				((x) == '[') || \
				((x) == ']') \
			)
/* e.g. fe80::1%eth0.620    vlan tag,
 *      fe80::1%eth0:0      IP alias
 *      fe80::1%qe0_0       Solaris link name
 *
 * The Linux kernel generally doesn't care too much, but everything else falls apart with
 * random characters in interface names.  Hyphen is a popular problematic character.
 */
#define IS_IP6_WITH_ZONE(x) ( \
				IS_IP6(x) || \
				((x) == '%') || \
				isalpha(x) || \
				((x) == '_') \
			    )
#define IS_NETPARAM(x) ( \
				((x) == ',') || \
				((x) == ';') \
			)

static inline
bool
is_network_char (
	const int		family,
	const char		c
	)
{
	if (IS_HOSTNAME(c) ||
	    (AF_INET == family && IS_IP(c)) ||
	    ((AF_INET6 == family || AF_UNSPEC == family) && IS_IP6_WITH_ZONE(c)) ||
	    IS_NETPARAM(c))
		return TRUE;
	else
		return FALSE;
}

static
bool
network_parse (
	const char*   restrict	network,		/* NULL terminated */
	int			family,			/* AF_UNSPEC | AF_INET | AF_INET6 */
	pgm_list_t**  restrict	recv_list,		/* <struct pgm_group_source_req*> */
	pgm_list_t**  restrict	send_list,		/* <struct pgm_group_source_req*> */
	pgm_error_t** restrict	error
	)
{
	bool retval = FALSE;
	const char *p = network;
	const char *e = p + strlen(network);
	enum { ENTITY_INTERFACE, ENTITY_RECEIVE, ENTITY_SEND, ENTITY_ERROR } ec = ENTITY_INTERFACE;
	const char *b = p;		/* begin of entity */
	pgm_list_t* source_list = NULL;
	pgm_error_t* sub_error = NULL;

/* pre-conditions */
	pgm_assert (NULL != network);
	pgm_assert (AF_UNSPEC == family || AF_INET == family || AF_INET6 == family);
	pgm_assert (NULL != recv_list);
	pgm_assert (NULL != send_list);

	pgm_debug ("network_parse (network:%s%s%s family:%s recv_list:%p send_list:%p error:%p)",
		network ? "\"" : "", network ? network : "(null)", network ? "\"" : "",
		pgm_family_string (family),
		(const void*)recv_list,
		(const void*)send_list,
		(const void*)error);

	while (p < e)
	{
		if (!is_network_char (family, *p))
		{
			pgm_set_error (error,
				     PGM_ERROR_DOMAIN_IF,
				     PGM_ERROR_INVAL,
				     _("'%c' is not a valid character."),
				     *p);
			goto free_lists;
		}

		if (*p == ';')		/* end of entity */
		{
			if (b == p)	/* empty entity */
			{
				switch (ec++) {
				case ENTITY_INTERFACE:
					retval = parse_interface_entity (family, NULL, &source_list, error);
					break;

				case ENTITY_RECEIVE:
					retval = parse_receive_entity (family, NULL, &source_list, recv_list, error);
					break;

				case ENTITY_SEND:
					retval = parse_send_entity (family, NULL, &source_list, recv_list, send_list, error);
					break;

				default:
					pgm_assert_not_reached();
					break;
				}

				if (!retval)
					goto free_lists;

				b = ++p;
				continue;
			}

/* entity from b to p-1 */
			char entity[1024];
			pgm_strncpy_s (entity, sizeof (entity), b, p - b);

			switch (ec++) {
			case ENTITY_INTERFACE:
				if (parse_interface_entity (family, entity, &source_list, &sub_error))
					break;
				if (!(sub_error && PGM_ERROR_XDEV == sub_error->code))
				{
/* fall through on multicast */
					if (!(sub_error && PGM_ERROR_NOTUNIQ == sub_error->code))
					{
						pgm_propagate_error (error, sub_error);
						goto free_lists;
					}
					pgm_clear_error (&sub_error);
/* FIXME: too many interfaces */
					if (pgm_list_length (source_list) > 1) {
						pgm_set_error (error,
				   			     PGM_ERROR_DOMAIN_IF,
							     PGM_ERROR_INVAL,
							     _("Send group list contains more than one entity."));
						goto free_lists;
					}
					break;
				}
				pgm_clear_error (&sub_error);
				while (source_list) {
					pgm_free (source_list->data);
					source_list = pgm_list_delete_link (source_list, source_list);
				}
				if (!parse_interface_entity (family, NULL, &source_list, &sub_error) &&
				    !(sub_error && PGM_ERROR_NOTUNIQ == sub_error->code))
				{
					pgm_propagate_error (error, sub_error);
					goto free_lists;
				}
				pgm_clear_error (&sub_error);
				ec++;
				/* fallthrough */

			case ENTITY_RECEIVE:
				if (!parse_receive_entity (family, entity, &source_list, recv_list, error))
					goto free_lists;
				break;

			case ENTITY_SEND:
				if (!parse_send_entity (family, entity, &source_list, recv_list, send_list, error))
					goto free_lists;
				break;

			default:
				pgm_assert_not_reached();
				break;
			}

			b = ++p;
			continue;
		}

		p++;
	}

	if (b < e) {
		switch (ec++) {
		case ENTITY_INTERFACE:
			if (parse_interface_entity (family, b, &source_list, &sub_error))
				break;
			if (!(sub_error && PGM_ERROR_XDEV == sub_error->code))
			{
/* fall through on multicast */
				if (!(sub_error && PGM_ERROR_NOTUNIQ == sub_error->code))
				{
					pgm_propagate_error (error, sub_error);
					goto free_lists;
				}
				pgm_clear_error (&sub_error);

/* FIXME: too many interfaces */
				if (pgm_list_length (source_list) > 1) {
					pgm_set_error (error,
			   			     PGM_ERROR_DOMAIN_IF,
						     PGM_ERROR_INVAL,
						     _("Send group list contains more than one entity."));
					goto free_lists;
				}
				break;
			}
			pgm_clear_error (&sub_error);
			while (source_list) {
				pgm_free (source_list->data);
				source_list = pgm_list_delete_link (source_list, source_list);
			}
			if (!parse_interface_entity (family, NULL, &source_list, &sub_error) &&
			    !(sub_error && PGM_ERROR_NOTUNIQ == sub_error->code))
			{
				pgm_propagate_error (error, sub_error);
				goto free_lists;
			}
			ec++;
			/* fallthrough */

		case ENTITY_RECEIVE:
			if (!parse_receive_entity (family, b, &source_list, recv_list, error))
				goto free_lists;
			break;

		case ENTITY_SEND:
			if (!parse_send_entity (family, b, &source_list, recv_list, send_list, error))
				goto free_lists;
			break;

		default:
			pgm_assert_not_reached();
			break;
		}
	}

	while (ec <= ENTITY_SEND)
	{
		switch (ec++) {
		case ENTITY_INTERFACE:
			if (!parse_interface_entity (family, NULL, &source_list, error))
				goto free_lists;
			break;

		case ENTITY_RECEIVE:
			if (!parse_receive_entity (family, NULL, &source_list, recv_list, error))
				goto free_lists;
			break;

		case ENTITY_SEND:
			if (!parse_send_entity (family, NULL, &source_list, recv_list, send_list, error))
				goto free_lists;
			break;

		default:
			pgm_assert_not_reached();
			break;
		}
	}

	if (pgm_list_length (source_list) > 1)
		goto free_lists;

/* cleanup source interface list */
	while (source_list) {
		pgm_free (source_list->data);
		source_list = pgm_list_delete_link (source_list, source_list);
	}

	return TRUE;

free_lists:
	while (source_list) {
		pgm_free (source_list->data);
		source_list = pgm_list_delete_link (source_list, source_list);
	}
	while (*recv_list) {
		pgm_free ((*recv_list)->data);
		*recv_list = pgm_list_delete_link (*recv_list, *recv_list);
	}
	while (*send_list) {
		pgm_free ((*send_list)->data);
		*send_list = pgm_list_delete_link (*send_list, *send_list);
	}
	return FALSE;
}

/* create pgm_group_source_req as used by pgm_transport_create which specify port, address & interface.
 * gsr_source is copied from gsr_group for ASM, caller needs to populate gsr_source for SSM.
 *
 * returns TRUE on success, returns FALSE on error and sets error appropriately.
 */

bool
pgm_getaddrinfo (
	const char*			   restrict network,
	const struct pgm_addrinfo_t* const restrict hints,
	struct pgm_addrinfo_t**		   restrict res,
	pgm_error_t**			   restrict error
	)
{
	struct pgm_addrinfo_t* ai;
	const int family = hints ? hints->ai_family : AF_UNSPEC;
	pgm_list_t* recv_list = NULL;	/* <struct pgm_group_source_req*> */
	pgm_list_t* send_list = NULL;	/* <struct pgm_group_source_req*> */

	pgm_return_val_if_fail (NULL != network, FALSE);
	pgm_return_val_if_fail (AF_UNSPEC == family || AF_INET == family || AF_INET6 == family, FALSE);
	pgm_return_val_if_fail (NULL != res, FALSE);

	if (hints) {
		pgm_debug ("pgm_getaddrinfo (network:%s%s%s hints: {family:%s} res:%p error:%p)",
			network ? "\"" : "", network ? network : "(null)", network ? "\"" : "",
			pgm_family_string (family),
			(const void*)res,
			(const void*)error);
	} else {
		pgm_debug ("pgm_getaddrinfo (network:%s%s%s hints:%p res:%p error:%p)",
			network ? "\"" : "", network ? network : "(null)", network ? "\"" : "",
			(const void*)hints,
			(const void*)res,
			(const void*)error);
	}

	if (!network_parse (network, family, &recv_list, &send_list, error))
		return FALSE;
	const size_t recv_list_len = pgm_list_length (recv_list);
	const size_t send_list_len = pgm_list_length (send_list);
	ai = pgm_malloc0 (sizeof(struct pgm_addrinfo_t) + 
			 (recv_list_len + send_list_len) * sizeof(struct pgm_group_source_req));
	ai->ai_recv_addrs_len = (uint32_t)recv_list_len;
	ai->ai_recv_addrs = (void*)((char*)ai + sizeof(struct pgm_addrinfo_t));
	ai->ai_send_addrs_len = (uint32_t)send_list_len;
	ai->ai_send_addrs = (void*)((char*)ai->ai_recv_addrs + recv_list_len * sizeof(struct pgm_group_source_req));

	size_t i = 0;
	while (recv_list) {
		memcpy (&ai->ai_recv_addrs[i++], recv_list->data, sizeof(struct pgm_group_source_req));
		pgm_free (recv_list->data);
		recv_list = pgm_list_delete_link (recv_list, recv_list);
	}
	i = 0;
	while (send_list) {
		memcpy (&ai->ai_send_addrs[i++], send_list->data, sizeof(struct pgm_group_source_req));
		pgm_free (send_list->data);
		send_list = pgm_list_delete_link (send_list, send_list);
	}
	*res = ai;
	return TRUE;
}

void
pgm_freeaddrinfo (
	struct pgm_addrinfo_t*	res
	)
{
	pgm_free (res);
}

/* eof */
