#!/usr/bin/env python3
"""Patch net/tipc/bearer.c to use the tipc_devmap shim (CONFIG_TIPC=n kernel)."""
import re
import sys

p = "bearer.c"
s = open(p).read()

if "tipc_devmap.h" not in s:
    s = s.replace('#include "bearer.h"',
                  '#include "bearer.h"\n'
                  '#include "tipc_devmap.h"   /* CONFIG_TIPC=n kernel: net_device member shim */',
                  1)

s = s.replace("rcu_assign_pointer(dev->tipc_ptr, b);", "tipc_devmap_set(dev, b);")
s = s.replace("RCU_INIT_POINTER(dev->tipc_ptr, NULL);", "tipc_devmap_clear(dev);")
s = re.sub(r"rcu_dereference\(dev->tipc_ptr\)", "tipc_devmap_get_rcu(dev)", s)
s = re.sub(r"rcu_dereference\(orig_dev->tipc_ptr\)", "tipc_devmap_get_rcu(orig_dev)", s)
s = s.replace("b = rtnl_dereference(dev->tipc_ptr);", "b = tipc_devmap_get_rtnl(dev);")

leftover = [ln for ln in s.splitlines() if "->tipc_ptr" in ln]
if leftover:
    print("UNPATCHED:", leftover)
    sys.exit(1)

open(p, "w").write(s)
print("bearer.c patched clean")
