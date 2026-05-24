# Netgraph as signal-routing table — design

Replace pub/sub semantics with **static per-signal routing**. Every
signal a node sends has a precompiled answer to "where does this go?"
— address baked at codegen time, fast lookup at runtime, no
discovery / wait / restart-dance.

## Problem statement

AUTOSAR ara::com pub/sub says: subscribe at startup, wait for
service-available, then send. That's nuked by the supervisor's
restart policy — every restart loses subscriptions, every partner has
to re-handshake. For an in-vehicle system whose state-machine is
already supervised + restart-deterministic, the pub/sub coupling adds
cost without adding value.

Static signal routing: at build time, every node's `signal → address`
table is committed. At runtime, send becomes a table lookup +
`cast(remote_ref, msg)`.

## What "address" means per layer

**TIPC is a CLUSTER protocol.** The kernel does node discovery over
Ethernet. A node has ONE address (`tipc type=...  instance=...`)
declared in `.art`, and that address resolves transparently
regardless of which machine the node ends up on. There's no
host-vs-cluster distinction at the wire level.

Two routing flavours only:

| Layer | Direction | Address shape |
|---|---|---|
| **TIPC** (in-cluster, kernel-discovered) | OUT | tipc/instance of the destination node |
| **PSP** (off-vehicle-bus) | OUT | `gateway` tipc/instance (gateway daemon translates to bus-side) |
| **PSP** | IN | tipc/instance of the gateway delivering the demuxed signal |
| **Bus-side address** | (consumed by gateway, not nodes) | CAN id, FlexRay slot+channel+cycle |

Key insights:
- **Outbound TIPC routing is just a destination tipc/instance.** No
  per-host vs per-cluster lookup — kernel does it.
- **Outbound PSP is ALWAYS `cast(gateway_ref, msg)`**. Bus-side
  details (CAN id / FlexRay slot) belong to the gateway's internal
  routing.
- **Instance-to-machine pinning is MANUAL** in rig.py — no automatic
  derivation from .art. The rig integrator decides which machine
  hosts which node-instance and which gets put in which executor
  subtree. The netgraph doesn't care about machines; the supervisor
  manifest does.

## Today's three generators

| Generator | What it emits | Plan |
|---|---|---|
| `artheia/generators/netgraph.py` | Per-node JSON with `gateway_routes` arrays | **KEEP, EXTEND**. Becomes the single source of truth for TIPC routing (per-node `signal → destinations[]`). Walks .art connect declarations + clusters. |
| `artheia/generators/host_netgraph.py` | `<Node>.<port> → tipc address` flat map | **DELETE / FOLD IN**. "Host" framing is wrong — there's no host-vs-cluster at the TIPC wire. The per-node info it produces is a subset of what netgraph.py emits. |
| `artheia/generators/netgraph_partition.py` | Per-bus `pdu → bus_address` from AUTOSAR catalog | **MOVE / RENAME to `gateway_netgraph.py`**. Bus-side addressing is gateway-internal — only the gateway daemon needs CAN id / FlexRay slot info. Apps never see it. |

The committed PSP netgraphs in `vendor/autosar/mlbevo_gen2_cmp_psp/system/mlbevo_gen2/{fibex,kcan}/netgraph.json` are catalog-shaped: `{ bus, bus_kind, routes: { signal: bus_address } }`. That format becomes the **gateway-only** netgraph — keep it (the gateway daemon already consumes it for its internal bus → TIPC translation), just rename the generator to make the role clear.

## Proposed format

One JSON per node, dotted into a single repo-wide aggregate. The runtime LUT is per-node so a node only ever loads its own slice.

```json
{
  "node": "sm",
  "tipc_type": "0x8001000d",
  "tipc_instance": 0,
  "signals": {
    "SmStateMsg": {
      "direction": "out",
      "destinations": [
        { "node": "exec", "tipc_type": "0x80010005", "tipc_instance": 0 },
        { "node": "com",  "tipc_type": "0x80010008", "tipc_instance": 0 },
        { "node": "ucm",  "tipc_type": "0x8001000e", "tipc_instance": 0 },
        { "node": "per",  "tipc_type": "0x80010007", "tipc_instance": 0 }
      ]
    },
    "SmRequest": {
      "direction": "in",
      "source_port": "ctl"
    }
  }
}
```

For a PSP-consuming app, the app netgraph is the same flat
destinations[] shape — outbound to a PSP signal casts to the
gateway's TIPC address, no bus-side detail in the app netgraph:

```json
{
  "node": "OddPathMonitor",
  "tipc_type": "0xc0010001",
  "tipc_instance": 0,
  "signals": {
    "EML_01": {
      "direction": "in",
      "destinations": [
        { "node": "gateway", "tipc_type": "0xa0010001", "tipc_instance": 0 }
      ]
    },
    "Trq_Req": {
      "direction": "out",
      "destinations": [
        { "node": "gateway", "tipc_type": "0xa0010001", "tipc_instance": 0 }
      ]
    }
  }
}
```

The bus-side (`can_id`, FlexRay `slot_id`/`channel_idx`/`cycle`)
lives in a **separate gateway netgraph** consumed only by the
gateway daemon (see `gateway_netgraph.py` below). Apps never see
CAN/FlexRay addressing.

Properties:

- **Destination-keyed within a signal**: `signal.destinations[]` for
  fan-out (sm → 4 partners). Multiple instances of one node type land
  here too: `[{node: foo, tipc_instance: 0}, {node: foo, tipc_instance: 1}]`.
- **One uniform "TIPC" destination shape**: just tipc_type + tipc_instance.
  No host-vs-cluster distinction — kernel resolves both.
- **PSP destination = gateway**: an outbound PSP signal's `destinations[]`
  is just the gateway's TIPC address. Bus translation is gateway-internal.

Lookup at runtime: `signal_name` → entry. `O(1)` if rendered as `std::unordered_map<std::string_view, Entry>` at construction. For codegen, a switch-statement on hashed signal name is even faster.

## Gateway netgraph (separate from app netgraph)

The gateway daemon needs `(pdu_name, bus_kind) → bus_address` to
translate between its TIPC inbound/outbound and the actual CAN/FlexRay
wire. That info is what `netgraph_partition.py` already produces;
keep it, rename the generator to make the role clear.

Format unchanged from today's PSP netgraphs:

```json
{
  "bus": "kcan",
  "bus_kind": "can",
  "routes": {
    "ACC_07": { "can_id": 302, "extended_id": false, "dlc": 8 }
  }
}
```

Only the gateway daemon consumes these. Apps consume per-node
netgraphs (above), which name the gateway as the TIPC destination
and stop there.

## Codegen strategy

One C++ header per node, alongside the gen-app lib:

```
services/system/sm/lib/sm_netgraph.hh    (new — gen-app extension)
```

Containing:

```cpp
namespace ara::sm::netgraph {

struct TipcAddr { uint32_t type; uint32_t instance; };

// Per-signal LUT, generated. Each entry is the precompiled answer
// to "where does <signal> go on this node?"
inline constexpr TipcAddr kSmStateMsg_destinations[] = {
    {0x80010005, 0},  // exec
    {0x80010008, 0},  // com
    {0x8001000e, 0},  // ucm
    {0x80010007, 0},  // per
};

}  // namespace ara::sm::netgraph
```

Then in `impl/`, `on_enter` does:

```cpp
for (auto addr : netgraph::kSmStateMsg_destinations) {
    cast_to(addr, msg);  // helper that builds a RemoteRef + casts
}
```

## Inputs come from `.art` only

The netgraph generator's input is the parsed .art model. **Not
rig.py.** Specifically:

- `.art node atomic <Name> { tipc type=... instance=... ports { ... } }`
  — gives the node's TIPC address + its port set.
- `.art composition C { prototype <Node> n  connect a.x to b.y }`
  — gives the architectural connect arrows. Each connect arrow
  becomes a per-signal destination entry on the source side.
- `.art cluster C { composition <Comp> c  connect a.x to b.y }`
  — cross-composition connects (formerly inter-process); same
  destination-entry shape, the netgraph doesn't care that the
  source and target are in different processes.

Manual rig.py work that the netgraph DOESN'T touch:
- Instance-to-machine pinning (which machine hosts node-instance N).
- Executor-subtree placement (which `core_sup` / `app_sup` branch
  the node lands under).
- Restart policy, scheduling priority, etc.

These are the rig integrator's calls — no automatic derivation
is possible from .art alone.

## Concrete next steps

1. **Audit `netgraph.py`** — survey gap between today's output and
   the per-node destination-keyed JSON above.
2. **Audit `host_netgraph.py`** — confirm everything it does is a
   subset of what `netgraph.py` now will do, then delete.
3. **Rename `netgraph_partition.py` → `gateway_netgraph.py`** —
   surface the role split.
4. **Implement the connect-walk** — for each composition/cluster
   ConnectDecl, append a destination to the source node's signal
   entry. Fan-out = multiple connects from one port.
5. **Pick build artifact location**: `platform/netgraph/<node>.json`
   (mirrors `platform/proto/` layout — one slice per node, aggregated
   by a BUILD.bazel filegroup).
6. **gen-app codegen** — extend the lib template to emit
   `<fc>_netgraph.hh` with constexpr TipcAddr[] tables.
7. **Runtime helper** — `cast_to(TipcAddr, Msg)` wraps the existing
   RemoteRef construct+cast.
8. **Migrate sm → 4-partner broadcast onto the netgraph LUT**. The
   in-process subscriber callback approach goes away; on_enter
   iterates the precompiled destinations[].

## Status

Plan committed. Step 1 (audit `netgraph.py`) starts next.
