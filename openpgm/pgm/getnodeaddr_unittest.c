/* vim:ts=8:sts=8:sw=4:noai:noexpandtab
 *
 * unit tests for portable function to return the nodes IP address.
 *
 * Copyright (c) 2009 Miru Limited.
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


#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>

#include <glib.h>
#include <check.h>

#include <pgm/getifaddrs.h>
#include <pgm/sockaddr.h>


/* mock state */

struct mock_host_t {
	struct sockaddr_storage	address;
	char*			canonical_hostname;
	char*			alias;
};

struct mock_interface_t {
	unsigned int		index;
	char*			name;
	unsigned int		flags;
	struct sockaddr_storage	addr;
	struct sockaddr_storage	netmask;
};

static GList *mock_hosts = NULL, *mock_interfaces = NULL;

#define MOCK_HOSTNAME		"kiku"
static char* mock_kiku =	MOCK_HOSTNAME;
static char* mock_localhost =	"localhost";
static char* mock_invalid =	"invalid.invalid";		/* RFC 2606 */
static char* mock_toolong =	"abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghij12345"; /* 65 */
static char* mock_hostname =	NULL;


static
gpointer
create_host (
	const char*	address,
	const char*	canonical_hostname,
	const char*	alias
	)
{
	struct mock_host_t* new_host;

	g_assert (address);
	g_assert (canonical_hostname);

	new_host = g_slice_alloc0 (sizeof(struct mock_host_t));
	g_assert (pgm_sockaddr_pton (address, &new_host->address));
	new_host->canonical_hostname = g_strdup (canonical_hostname);
	new_host->alias = alias ? g_strdup (alias) : NULL;

	return new_host;
}

static
gpointer
create_interface (
	const unsigned	index,
	const char*	name,
	const char*	flags
	)
{
	struct mock_interface_t* new_interface;

	g_assert (name);
	g_assert (flags);

	new_interface = g_slice_alloc0 (sizeof(struct mock_interface_t));
	new_interface->index = index;
	new_interface->name = g_strdup (name);

	struct sockaddr_in* sin = (gpointer)&new_interface->addr;
	struct sockaddr_in6* sin6 = (gpointer)&new_interface->addr;

	gchar** tokens = g_strsplit (flags, ",", 0);
	for (guint i = 0; tokens[i]; i++)
	{
		if (strcmp (tokens[i], "up") == 0)
			new_interface->flags |= IFF_UP;
		else if (strcmp (tokens[i], "down") == 0)
			new_interface->flags |= 0;
		else if (strcmp (tokens[i], "loop") == 0)
			new_interface->flags |= IFF_LOOPBACK;
		else if (strcmp (tokens[i], "broadcast") == 0)
			new_interface->flags |= IFF_BROADCAST;
		else if (strcmp (tokens[i], "multicast") == 0)
			new_interface->flags |= IFF_MULTICAST;
		else if (strncmp (tokens[i], "ip=", strlen("ip=")) == 0) {
			const char* addr = tokens[i] + strlen("ip=");
			g_assert (pgm_sockaddr_pton (addr, &new_interface->addr));
		}
		else if (strncmp (tokens[i], "netmask=", strlen("netmask=")) == 0) {
			const char* addr = tokens[i] + strlen("netmask=");
			g_assert (pgm_sockaddr_pton (addr, &new_interface->netmask));
		}
		else if (strncmp (tokens[i], "scope=", strlen("scope=")) == 0) {
			const char* scope = tokens[i] + strlen("scope=");
			g_assert (AF_INET6 == ((struct sockaddr*)&new_interface->addr)->sa_family);
			((struct sockaddr_in6*)&new_interface->addr)->sin6_scope_id = atoi (scope);
		}
		else
			g_error ("parsing failed for flag \"%s\"", tokens[i]);
	}
			
	g_strfreev (tokens);
	return new_interface;
}

#define APPEND_HOST2(a,b,c)	\
		do { \
			gpointer data = create_host ((a), (b), (c)); \
			g_assert (data); \
			mock_hosts = g_list_append (mock_hosts, data); \
			g_assert (mock_hosts); g_assert (mock_hosts->data); \
		} while (0)
#define APPEND_HOST(a,b)	APPEND_HOST2((a),(b),NULL)
#define APPEND_INTERFACE(a,b,c)	\
		do { \
			gpointer data = create_interface ((a), (b), (c)); \
			g_assert (data); \
			mock_interfaces = g_list_append (mock_interfaces, data); \
			g_assert (mock_interfaces); g_assert (mock_interfaces->data); \
		} while (0)
static
void
mock_setup_net (void)
{
	mock_hostname = mock_kiku;

	APPEND_HOST (	"127.0.0.1",		"localhost");
	APPEND_HOST2(	"10.6.28.33",		"kiku.hk.miru.hk",	"kiku");
	APPEND_HOST2(	"2002:dce8:d28e::33",	"ip6-kiku",		"kiku");
	APPEND_HOST2(	"::1",			"ip6-localhost",	"ip6-loopback");

	APPEND_INTERFACE(	1,	"lo",	"up,loop");
	APPEND_INTERFACE(	2,	"eth0",	"up,broadcast,multicast");
	APPEND_INTERFACE(	3,	"eth1",	"down,broadcast,multicast");
	APPEND_INTERFACE(	1,	"lo",	"up,loop,ip=127.0.0.1,netmask=255.0.0.0");
	APPEND_INTERFACE(	2,	"eth0",	"up,broadcast,multicast,ip=10.6.28.33,netmask=255.255.255.0");
	APPEND_INTERFACE(	1,	"lo",	"up,loop,ip=::1,netmask=ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff,scope=0");
	APPEND_INTERFACE(	2,	"eth0",	"up,broadcast,multicast,ip=2002:dce8:d28e::33,netmask=ffff:ffff:ffff:ffff::0,scope=0");
	APPEND_INTERFACE(	2,	"eth0",	"up,broadcast,multicast,ip=fe80::214:5eff:febd:6dda,netmask=ffff:ffff:ffff:ffff::0,scope=2");
}

/* with broken IPv6 hostname setup */
static
void
mock_setup_net2 (void)
{
	mock_hostname = mock_kiku;

	APPEND_HOST (	"127.0.0.1",		"localhost");
	APPEND_HOST2(	"10.6.28.33",		"kiku.hk.miru.hk",	"kiku");
	APPEND_HOST(	"2002:dce8:d28e::33",	"ip6-kiku");
	APPEND_HOST2(	"::1",			"ip6-localhost",	"ip6-loopback");

	APPEND_INTERFACE(	1,	"lo",	"up,loop");
	APPEND_INTERFACE(	2,	"eth0",	"up,broadcast,multicast");
	APPEND_INTERFACE(	3,	"eth1",	"down,broadcast,multicast");
	APPEND_INTERFACE(	1,	"lo",	"up,loop,ip=127.0.0.1,netmask=255.0.0.0");
	APPEND_INTERFACE(	2,	"eth0",	"up,broadcast,multicast,ip=10.6.28.33,netmask=255.255.255.0");
	APPEND_INTERFACE(	1,	"lo",	"up,loop,ip=::1,netmask=ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff,scope=0");
	APPEND_INTERFACE(	2,	"eth0",	"up,broadcast,multicast,ip=2002:dce8:d28e::33,netmask=ffff:ffff:ffff:ffff::0,scope=0");
	APPEND_INTERFACE(	2,	"eth0",	"up,broadcast,multicast,ip=fe80::214:5eff:febd:6dda,netmask=ffff:ffff:ffff:ffff::0,scope=2");
}

static
void
mock_teardown_net (void)
{
	GList* list;

	list = mock_hosts;
	while (list) {
		struct mock_host_t* host = list->data;
		g_free (host->canonical_hostname);
		if (host->alias)
			g_free (host->alias);
		g_slice_free1 (sizeof(struct mock_host_t), host);
		list = list->next;
	}
	g_list_free (mock_hosts);

	list = mock_interfaces;
	while (list) {
		struct mock_interface_t* interface = list->data;
		g_free (interface->name);
		g_slice_free1 (sizeof(struct mock_interface_t), interface);
		list = list->next;
	}
	g_list_free (mock_interfaces);
}


/* mock functions for external references */

static 
int
mock_getifaddrs (
	struct ifaddrs**	ifap
	)
{
	if (NULL == ifap) {
		errno = EINVAL;
		return -1;
	}

	g_debug ("mock_getifaddrs (ifap:%p)", (gpointer)ifap);

	GList* list = mock_interfaces;
	int n = g_list_length (list);
	struct ifaddrs* ifa = malloc (n * sizeof(struct ifaddrs));
	memset (ifa, 0, n * sizeof(struct ifaddrs));
	struct ifaddrs* ift = ifa;
	while (list) {
		struct mock_interface_t* interface = list->data;
		ift->ifa_addr = (gpointer)&interface->addr;
		ift->ifa_name = interface->name;
		ift->ifa_flags = interface->flags;
		ift->ifa_netmask = (gpointer)&interface->netmask;
		list = list->next;
		if (list) {
			ift->ifa_next = ift + 1;
			ift = ift->ifa_next;
		}
	}

	*ifap = ifa;

	return 0;
}

static
void
mock_freeifaddrs (
	struct ifaddrs*		ifa
	)
{
	g_debug ("mock_freeifaddrs (ifa:%p)", (gpointer)ifa);
	free (ifa);
}

static
struct hostent*
mock_gethostbyname (
	const char*		name
	)
{
	static struct hostent he;
	static char* aliases[2];
	static char* addr_list[2];

/* pre-conditions */
	g_assert (NULL != name);

	g_debug ("mock_gethostbyname (name:\"%s\")", name);

	GList* list = mock_hosts;
	while (list) {
		struct mock_host_t* host = list->data;
		const int host_family = ((struct sockaddr*)&host->address)->sa_family;
		if (((strcmp (host->canonical_hostname, name) == 0) ||
		     (host->alias && strcmp (host->alias, name) == 0)))
		{
			he.h_name	= host->canonical_hostname;
			aliases[0] = host->alias;
			aliases[1] = NULL;
			he.h_aliases	= aliases;
			he.h_addrtype	= host_family;
			he.h_length	= pgm_sockaddr_addr_len (&host->address);
			addr_list[0] = (char*)pgm_sockaddr_addr (&host->address);
			addr_list[1] = NULL;
			he.h_addr_list	= addr_list;
			return &he;
		}
		list = list->next;
	}
	h_errno = HOST_NOT_FOUND;
	return NULL;
}

static
int
mock_getaddrinfo (
	const char*		node,
	const char*		service,
	const struct addrinfo*	hints,
	struct addrinfo**	res
	)
{
	const int ai_flags  = hints ? hints->ai_flags  : (AI_V4MAPPED | AI_ADDRCONFIG);
	const int ai_family = hints ? hints->ai_family : AF_UNSPEC;
	GList* list;
	struct sockaddr_storage addr;

	if (NULL == node && NULL == service)
		return EAI_NONAME;

/* pre-conditions */
	g_assert (NULL != node);
	g_assert (NULL == service);
	g_assert (!(ai_flags & AI_CANONNAME));
	g_assert (!(ai_flags & AI_NUMERICSERV));
	g_assert (!(ai_flags & AI_V4MAPPED));

	g_debug ("mock_getaddrinfo (node:\"%s\" service:%s hints:%p res:%p)",
		node, service, (gpointer)hints, (gpointer)res);

	gboolean has_ip4_config;
	gboolean has_ip6_config;

	if (hints && hints->ai_flags & AI_ADDRCONFIG)
	{
		has_ip4_config = has_ip6_config = FALSE;
		list = mock_interfaces;
		while (list) {
			const struct mock_interface_t* interface = list->data;
			if (AF_INET == ((struct sockaddr*)&interface->addr)->sa_family)
				has_ip4_config = TRUE;
			else if (AF_INET6 == ((struct sockaddr*)&interface->addr)->sa_family)
				has_ip6_config = TRUE;
			if (has_ip4_config && has_ip6_config)
				break;
			list = list->next;
		}
	} else {
		has_ip4_config = has_ip6_config = TRUE;
	}

	if (ai_flags & AI_NUMERICHOST) {
		pgm_sockaddr_pton (node, &addr);
	}
	list = mock_hosts;
	while (list) {
		struct mock_host_t* host = list->data;
		const int host_family = ((struct sockaddr*)&host->address)->sa_family;
		if (((strcmp (host->canonical_hostname, node) == 0) ||
		     (host->alias && strcmp (host->alias, node) == 0) ||
		     (ai_flags & AI_NUMERICHOST &&
		      0 == pgm_sockaddr_cmp ((struct sockaddr*)&addr, (struct sockaddr*)&host->address)))
		     &&
		    (host_family == ai_family || AF_UNSPEC == ai_family) &&
		    ((AF_INET == host_family && has_ip4_config) || (AF_INET6 == host_family && has_ip6_config)))
		{
			struct addrinfo* ai = malloc (sizeof(struct addrinfo));
			memset (ai, 0, sizeof(struct addrinfo));
			ai->ai_family = host_family;
			ai->ai_addrlen = AF_INET == host_family ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
			ai->ai_addr = (gpointer)&host->address;
			*res = ai;
			return 0;
		}
		list = list->next;
	}
	return EAI_NONAME;
}

static
void
mock_freeaddrinfo (
	struct addrinfo*	res
	)
{
	g_assert (NULL != res);
	g_debug ("mock_freeaddrinfo (res:%p)", (gpointer)res);
	free (res);
}

static
int
mock_gethostname (
	char*			name,
	size_t			len
	)
{
	g_debug ("mock_gethostname (name:%p len:%d)",
		(gpointer)name, len);

	if (NULL == name) {
		errno = EFAULT;
		return -1;
	}
	if (len < 0) {
		errno = EINVAL;
		return -1;
	}
	if (len < (1 + strlen (mock_hostname))) {
		errno = ENAMETOOLONG;
		return -1;
	}
/* force an error */
	if (mock_hostname == mock_toolong) {
		errno = ENAMETOOLONG;
		return -1;
	}
	strncpy (name, mock_hostname, len);
	if (len > 0)
		name[len - 1] = '\0';
	return 0;
}

#define getifaddrs	mock_getifaddrs
#define freeifaddrs	mock_freeifaddrs
#define getaddrinfo	mock_getaddrinfo
#define freeaddrinfo	mock_freeaddrinfo
#define gethostname	mock_gethostname
#define gethostbyname	mock_gethostbyname


#define GETNODEADDR_DEBUG
#include "getnodeaddr.c"


/* target:
 *	gboolean
 *	_pgm_if_getnodeaddr (
 *		const int		family,
 *		struct sockaddr*	addr,
 *		const socklen_t		cnt,
 *		GError**		error
 *	)
 */

START_TEST (test_getnodeaddr_pass_001)
{
	struct sockaddr_storage addr;
	char saddr[INET6_ADDRSTRLEN];
	GError* err = NULL;
	gboolean success = _pgm_if_getnodeaddr (AF_UNSPEC, (struct sockaddr*)&addr, sizeof(addr), &err);
	if (!success && err) {
		g_error ("Resolving node address with AF_UNSPEC: %s", err->message);
	}
	fail_unless (TRUE == success);
	fail_unless (NULL == err);
	pgm_sockaddr_ntop ((struct sockaddr*)&addr, saddr, sizeof(saddr));
	g_message ("AF_UNSPEC:%s", saddr);
	fail_unless (TRUE == _pgm_if_getnodeaddr (AF_INET, (struct sockaddr*)&addr, sizeof(addr), &err));
	fail_unless (NULL == err);
	pgm_sockaddr_ntop ((struct sockaddr*)&addr, saddr, sizeof(saddr));
	g_message ("AF_INET:%s", saddr);
	fail_unless (TRUE == _pgm_if_getnodeaddr (AF_INET6, (struct sockaddr*)&addr, sizeof(addr), &err));
	fail_unless (NULL == err);
	pgm_sockaddr_ntop ((struct sockaddr*)&addr, saddr, sizeof(saddr));
	g_message ("AF_INET6:%s", saddr);
}
END_TEST

START_TEST (test_getnodeaddr_fail_001)
{
	GError* err = NULL;
	fail_unless (FALSE == _pgm_if_getnodeaddr (AF_UNSPEC, NULL, 0, &err));
}
END_TEST


static
Suite*
make_test_suite (void)
{
	Suite* s;

	s = suite_create (__FILE__);

	TCase* tc_getnodeaddr = tcase_create ("getnodeaddr");
	suite_add_tcase (s, tc_getnodeaddr);
	tcase_add_checked_fixture (tc_getnodeaddr, mock_setup_net, mock_teardown_net);
	tcase_add_test (tc_getnodeaddr, test_getnodeaddr_pass_001);
	tcase_add_test (tc_getnodeaddr, test_getnodeaddr_fail_001);

	TCase* tc_getnodeaddr2 = tcase_create ("getnodeaddr/2");
	suite_add_tcase (s, tc_getnodeaddr2);
	tcase_add_checked_fixture (tc_getnodeaddr2, mock_setup_net2, mock_teardown_net);
	tcase_add_test (tc_getnodeaddr2, test_getnodeaddr_pass_001);
	return s;
}

static
Suite*
make_master_suite (void)
{
	Suite* s = suite_create ("Master");
	return s;
}

int
main (void)
{
	SRunner* sr = srunner_create (make_master_suite ());
	srunner_add_suite (sr, make_test_suite ());
	srunner_run_all (sr, CK_ENV);
	int number_failed = srunner_ntests_failed (sr);
	srunner_free (sr);
	return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

/* eof */
