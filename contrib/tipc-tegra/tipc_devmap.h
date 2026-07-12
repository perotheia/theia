/* tipc_devmap.h — out-of-tree shim for kernels built with CONFIG_TIPC=n.
 *
 * struct net_device::tipc_ptr is #if IS_ENABLED(CONFIG_TIPC)-guarded in
 * netdevice.h, so on a kernel compiled without TIPC (NVIDIA L4T/tegra) the
 * member does not exist and an external tipc.ko cannot reference it. Only
 * bearer.c uses it (4 sites: attach, detach, rx-lookup, rtnl-lookup) — this
 * shim replaces it with a small RCU side table keyed by the net_device
 * POINTER (namespace-safe by construction; ifindex reuse can't alias).
 *
 * Same approach proven on the Jetson AGX (5.10.104-tegra, 2026-06-26); this
 * is the 5.15.148-tegra (Orin) recreation. Writers (enable/disable, rtnl
 * serialized) take a spinlock; the rx hot path is a lock-free RCU read.
 */
#ifndef _TIPC_DEVMAP_H
#define _TIPC_DEVMAP_H

#include <linux/netdevice.h>
#include <linux/rcupdate.h>
#include <linux/spinlock.h>

#define TIPC_DEVMAP_SLOTS 16   /* a board has a handful of bearers at most */

struct tipc_devmap_ent {
	const struct net_device *dev;      /* NULL = free slot */
	struct tipc_bearer __rcu *bearer;
};

static struct tipc_devmap_ent tipc_devmap[TIPC_DEVMAP_SLOTS];
static DEFINE_SPINLOCK(tipc_devmap_lock);

/* attach: dev->tipc_ptr = b  (rcu_assign_pointer equivalent) */
static inline void tipc_devmap_set(const struct net_device *dev,
				   struct tipc_bearer *b)
{
	int i, free_slot = -1;

	spin_lock_bh(&tipc_devmap_lock);
	for (i = 0; i < TIPC_DEVMAP_SLOTS; i++) {
		if (tipc_devmap[i].dev == dev) {
			rcu_assign_pointer(tipc_devmap[i].bearer, b);
			spin_unlock_bh(&tipc_devmap_lock);
			return;
		}
		if (!tipc_devmap[i].dev && free_slot < 0)
			free_slot = i;
	}
	if (free_slot >= 0) {
		rcu_assign_pointer(tipc_devmap[free_slot].bearer, b);
		smp_wmb();  /* bearer visible before the slot goes live */
		WRITE_ONCE(tipc_devmap[free_slot].dev, dev);
	}
	spin_unlock_bh(&tipc_devmap_lock);
}

/* detach: dev->tipc_ptr = NULL (slot freed; readers see NULL bearer first) */
static inline void tipc_devmap_clear(const struct net_device *dev)
{
	int i;

	spin_lock_bh(&tipc_devmap_lock);
	for (i = 0; i < TIPC_DEVMAP_SLOTS; i++) {
		if (tipc_devmap[i].dev == dev) {
			RCU_INIT_POINTER(tipc_devmap[i].bearer, NULL);
			WRITE_ONCE(tipc_devmap[i].dev, NULL);
			break;
		}
	}
	spin_unlock_bh(&tipc_devmap_lock);
}

/* lookup: rcu_dereference(dev->tipc_ptr) — rx hot path, RCU read side only */
static inline struct tipc_bearer *tipc_devmap_get_rcu(const struct net_device *dev)
{
	int i;

	for (i = 0; i < TIPC_DEVMAP_SLOTS; i++) {
		if (READ_ONCE(tipc_devmap[i].dev) == dev)
			return rcu_dereference(tipc_devmap[i].bearer);
	}
	return NULL;
}

/* lookup under rtnl: rtnl_dereference(dev->tipc_ptr) */
static inline struct tipc_bearer *tipc_devmap_get_rtnl(const struct net_device *dev)
{
	int i;

	for (i = 0; i < TIPC_DEVMAP_SLOTS; i++) {
		if (READ_ONCE(tipc_devmap[i].dev) == dev)
			return rtnl_dereference(tipc_devmap[i].bearer);
	}
	return NULL;
}

#endif /* _TIPC_DEVMAP_H */
