/*
 * Copyright (c) 2014 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * Use is subject to license terms, as specified in the LICENSE file.
 */

/*
 * NPF network interface handling module.
 */

#ifdef _KERNEL
#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/types.h>
#include <sys/kmem.h>

#include <net/if.h>
#include <netinet/in.h>
#include <netinet6/in6_var.h>
#endif

#include "npf_impl.h"

static npf_table_t *
lookup_ifnet_table(npf_t *npf, ifnet_t *ifp)
{
	const npf_ifops_t *ifops = npf->ifops;
	char tname[NPF_TABLE_MAXNAMELEN];
	const char *ifname;
	npf_config_t *nc;
	npf_table_t *t;
	unsigned tid;

	/* Get the interface name and prefix it. */
	ifname = ifops->getname(npf, ifp);
	snprintf(tname, sizeof(tname), ".ifnet-%s", ifname);

	KERNEL_LOCK(1, NULL);
	nc = npf_config_enter(npf);

	/*
	 * Check whether this interface is of any interest to us.
	 */
	t = npf_tableset_getbyname(nc->tableset, tname);
	if (!t) {
		goto out;
	}
	tid = npf_table_getid(t);

	/* Create a new NPF table for the interface. */
	t = npf_table_create(tname, tid, NPF_TABLE_IFADDR, NULL, 0);
	if (!t) {
		goto out;
	}
	return t;
out:
	npf_config_exit(npf);
	KERNEL_UNLOCK_ONE(NULL);
	return NULL;
}

static void
replace_ifnet_table(npf_t *npf, npf_table_t *newt)
{
	npf_tableset_t *ts = npf->config->tableset;
	npf_table_t *oldt;

	KERNEL_UNLOCK_ONE(NULL);

	/*
	 * Finally, swap the tables and issue a sync barrier.
	 */
	oldt = npf_tableset_swap(ts, newt);
	npf_config_sync(npf);
	npf_config_exit(npf);

	/* At this point, it is safe to destroy the old table. */
	npf_table_destroy(oldt);
}

void
npf_ifaddr_sync(npf_t *npf, ifnet_t *ifp)
{
	npf_table_t *t;
	struct ifaddr *ifa;

	/*
	 * First, check whether this interface is of any interest to us.
	 *
	 * => Acquires npf-config-lock and kernel-lock on success.
	 */
	t = lookup_ifnet_table(npf, ifp);
	if (!t)
		return;

	/*
	 * Populate the table with the interface addresses.
	 * Note: currently, this list is protected by the kernel-lock.
	 */
	IFADDR_FOREACH(ifa, ifp) {
		struct sockaddr *sa = ifa->ifa_addr;
		const void *p = NULL;
		int alen = 0;

		if (sa->sa_family == AF_INET) {
			const struct sockaddr_in *sin4 = satosin(sa);
			alen = sizeof(struct in_addr);
			p = &sin4->sin_addr;
		}
		if (sa->sa_family == AF_INET6) {
			const struct sockaddr_in6 *sin6 = satosin6(sa);
			alen = sizeof(struct in6_addr);
			p = &sin6->sin6_addr;
		}
		if (alen) {
			npf_addr_t addr;
			memcpy(&addr, p, alen);
			npf_table_insert(t, alen, &addr, NPF_NO_NETMASK);
		}
	}

	/* Publish the new table. */
	replace_ifnet_table(npf, t);
}

void
npf_ifaddr_flush(npf_t *npf, ifnet_t *ifp)
{
	npf_table_t *t;

	/*
	 * Flush: just load an empty table.
	 */
	t = lookup_ifnet_table(npf, ifp);
	if (!t) {
		return;
	}
	replace_ifnet_table(npf, t);
}

void
npf_ifaddr_syncall(npf_t *npf)
{
	ifnet_t *ifp;

	KERNEL_LOCK(1, NULL);
	IFNET_GLOBAL_LOCK();
	IFNET_WRITER_FOREACH(ifp) {
		npf_ifaddr_sync(npf, ifp);
	}
	IFNET_GLOBAL_UNLOCK();
	KERNEL_UNLOCK_ONE(NULL);
}
