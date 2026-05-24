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

| Layer | Direction | Address shape |
|---|---|---|
| **PSP** (off-host bus traffic) | OUT | `gateway` tipc/instance — outgoing always routes through the on-host gateway, which translates to bus-side |
| **PSP** | IN | tipc/instance of the gateway delivering the demuxed signal |
| **Host** (intra-machine) | OUT | tipc/instance of the destination node |
| **Cluster** (inter-machine) | OUT | tipc/instance of the destination node + machine route |
| **Inbound bus addressing** | (consumed by gateway, not nodes) | CAN id, FlexRay slot+channel+cycle |

Key insight: **outbound to a PSP signal is ALWAYS `cast(gateway_ref, msg)`**. The bus-side details (CAN id / FlexRay slot) belong to the gateway's internal routing, not the application's table. Application nodes only see TIPC addresses.

## Today's three generators

| Generator | What it emits | Status |
|---|---|---|
| `artheia/generators/netgraph.py` | Per-node JSON with `gateway_routes` arrays | Mixed; partial |
| `artheia/generators/host_netgraph.py` | `<Node>.<port> → tipc address` flat map | Host-only; close to right shape |
| `artheia/generators/netgraph_partition.py` | Per-bus `pdu → bus_address` from AUTOSAR catalog | Catalog-shaped (the "wrong format" we hit); is per-bus, not per-node-signal |

The committed PSP netgraphs in `vendor/autosar/mlbevo_gen2_cmp_psp/system/mlbevo_gen2/{fibex,kcan}/netgraph.json` are catalog-shaped: `{ bus, bus_kind, routes: { signal: bus_address } }`. **Not destination-keyed.**

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

For a PSP signal:

```json
{
  "node": "OddPathMonitor",
  "tipc_type": "0xc0010001",
  "tipc_instance": 0,
  "signals": {
    "EML_01": {
      "direction": "in",
      "via_gateway": { "node": "gateway", "tipc_type": "0xa0010001", "tipc_instance": 0 },
      "bus": "mlbevo_gen2",
      "bus_kind": "flexray",
      "slot_id": 5, "channel_idx": 0, "cycle": 15
    },
    "Trq_Req": {
      "direction": "out",
      "via_gateway": { "node": "gateway", "tipc_type": "0xa0010001", "tipc_instance": 0 },
      "bus": "kcan",
      "bus_kind": "can",
      "can_id": 320
    }
  }
}
```

Properties:

- **Destination-keyed within a signal**: `signal.destinations[]` for fan-out (sm → 4 partners). Multiple instances of one node type land here too: `[{node: foo, tipc_type, tipc_instance: 0}, {node: foo, tipc_type, tipc_instance: 1}]`.
- **PSP outbound = `via_gateway` field**: the destination address an outbound PSP signal lands at on TIPC. Bus-side (CAN id, FlexRay slot) is metadata for the gateway, the app casts to `via_gateway` and stops there.
- **PSP inbound**: similarly `via_gateway` is who delivers it — the gateway demuxes from the bus and casts to the receiving node.
- **Host outbound**: just `destinations[]` of TIPC addresses.
- **Cluster outbound**: same `destinations[]`; the runtime joins against the machine-manifest to resolve cross-machine TIPC routes (TIPC kernel handles inter-host already; the app doesn't see the difference).

Lookup at runtime: `signal_name` → entry. `O(1)` if rendered as `std::unordered_map<std::string_view, Entry>` at construction. For codegen, a switch-statement on hashed signal name is even faster.

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

## Concrete next steps

1. **Audit existing three generators**: which one comes closest, what gaps remain.
2. **Decide format**: confirm destination-keyed per-node JSON above is right.
3. **Pick the build artifact location**: `platform/netgraph/<node>.json`? `services/system/<fc>/netgraph.json`?
4. **Rig.py is the input**: rig knows which nodes exist, which machines they live on, which signals they emit/consume. The netgraph generator reads the rig + .art models + emits per-node tables.
5. **Codegen**: extend gen-app to emit `<fc>_netgraph.hh` from the per-node netgraph entry. Hook in lib/.
6. **Runtime helper**: add `cast_to(TipcAddr, Msg)` to platform/runtime — wraps the existing RemoteRef construct+cast.
7. **Migrate sm → 4-partner broadcast off the in-process subscriber callback** onto the netgraph LUT. The orchestrator test grows a cross-process variant.

## Status

Plan committed. Step 1 (audit) starts next.
