# Requirements: functional vs non-functional

The hard distinction that governs every design and refactor decision on this
project. Read this before "simplifying", removing, or rewriting any code.

## Two kinds of requirement

**FUNCTIONAL** — what the system needs to **perform its function**. The whole
data path counts: `cmp_gw → gateway → AUTOSAR FIBEX/DBC parsing`; the bus
mega-nodes + their per-PDU interfaces; the supervisor forking children; trace
egress; the config gatekeeper storing + migrating config. **Non-negotiable —
implement them.** A functional requirement is binary: it works, or the system
can't do its job.

**NON-FUNCTIONAL** — speed (e.g. textX O(N²) on 1025 PDUs), security, UX,
elegance. Important, but **almost always a compromise**: implement / optimize /
work around until **good enough**. There is no perfect non-functional system —
chasing perfection here is wasted effort.

Accepted compromises (good enough, on purpose):
- the bus `component.art`'s N² parse (~4.5s) — gated behind `--force` / a
  node-only default view.
- trace config is `tdb → supervisor` (no `com` relay) — slightly less
  convenient, fine.
- per's snapshot is config-prefix scoped, not a full etcd backup — safe + fast
  for a config gatekeeper, the full backup is an ops concern.

## The rule

When a **functional** impl has a **non-functional** problem:
**keep the function, optimize around the problem.** Lazy resolution, projection,
a `--force` gate, a bounded budget, a deferred fan-out — find another way to pay
the non-functional cost. **Never strip the function** to make a non-functional
concern go away.

Before removing or "simplifying" code, ask: **is this a functional part of the
data path?** If yes, it stays.

## The failure mode to never repeat

An agent once **deleted a working functional implementation** — the AUTOSAR bus
nodes + per-PDU interfaces in the PSP — to satisfy a non-functional concern (the
O(N²) parse). That's backwards. The whole psp-bus-node session became
**regression recovery**: rebuilding state we already had. The non-functional
problem was real, but the fix was a catalog/lazy-parse projection, not deleting
the bus nodes.

## See also

- The recovered impl + its lazy-parse fix: `project-psp-bus-node-catalog-fallback`
  memory.
- Reuse, don't strip (the runtime): `feedback-dont-rewrite-runtime` memory.
