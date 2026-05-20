# PERO CMP — system architecture from the Artheia modeling perspective

How a vendor application talks to a CAN/FlexRay bus through the PERO
gateway, and how every artefact between FIBEX/DBC source and the host
runtime LUT is produced.

This document is the spec the code generators read from. If you are
adding a generator (a new wire-format emitter, a runtime header writer,
a Bazel rule), start here.

## TL;DR

```mermaid
flowchart LR
  classDef src   fill:#fff5cc,stroke:#b58900,color:#222
  classDef cat   fill:#e0f0ff,stroke:#268bd2,color:#222
  classDef art   fill:#e0ffe0,stroke:#859900,color:#222
  classDef json  fill:#ffe0f0,stroke:#d33682,color:#222
  classDef code  fill:#f0f0f0,stroke:#444,color:#222

  fibex[FIBEX K-Matrix .xml]:::src
  dbc[DBC files]:::src

  fibex   -->|artheia import-fibex| pkgFib[autosar/&lt;psp&gt;/system/mlbevo_gen2/<br/>package.art + catalog.json]:::cat
  dbc     -->|artheia import-dbc|   pkgCan[autosar/&lt;psp&gt;/system/kcan/<br/>package.art + catalog.json]:::cat

  pkgFib  -->|gen-netgraph-partition| ngFib[mlbevo_gen2/netgraph.json]:::json
  pkgCan  -->|gen-netgraph-partition| ngCan[kcan/netgraph.json]:::json

  pkgFib  -->|gen-autosar-system|    autosys[autosar/&lt;psp&gt;/system/system.art<br/>2 bus mega-nodes, ~1500 sender ports]:::art
  pkgCan  --> autosys

  autosys -.fwd-decl.-> gwsys[gateway/system/package.art<br/>Gateway TIPC node + GatewayBridge composition]:::art
  gwsys   -.fwd-decl.-> platsys[platform/system/system.art<br/>Platform composition + symlinks]:::art

  vendor[vendor/&lt;v&gt;/system/<br/>components + interfaces] -.import.-> platsys

  platsys -->|gen-host-netgraph| hostng[platform/config/host_netgraph.json<br/>symbolic_port → TIPC addr]:::json
  ngFib   --> idx[platform/config/netgraph.cfg<br/>index of all partitions]:::json
  ngCan   --> idx
  hostng  --> idx

  idx --> cg[code generators<br/>e.g. gw runtime LUT, signal_filter.csv]:::code
```

## 1. Layers

The model is split into **layers**. Each layer owns one concern; layers
combine via cross-file `import`s and symlinks. The layering matters: a
codegen tool can stop at the layer it needs and ignore everything below.

| Layer | Owns | Lives in | Authoritative artefact |
| --- | --- | --- | --- |
| **Source** | OEM-provided wire definitions | OEM-shipped files | `*.xml` (FIBEX), `*.dbc` |
| **AUTOSAR catalog** | Per-bus PDU shape (fields, types, enums) | `autosar/<psp>/system/<bus>/` | `catalog.json` + `package.art` |
| **AUTOSAR netgraph** | Per-bus PDU → wire address LUT | `autosar/<psp>/system/<bus>/` | `netgraph.json` |
| **AUTOSAR system** | All buses as mega-nodes with PDU ports | `autosar/<psp>/system/` | `system.art` |
| **Gateway** | TIPC endpoint + bus prototypes | `gateway/system/` | `package.art` |
| **Vendor app** | App's component(s), ports, interfaces | `vendor/<v>/system/` | `package.art`, `components/*.art`, `system.art` |
| **Platform** | Top-level composition + host netgraph | `platform/system/`, `platform/config/` | `system.art`, `host_netgraph.json`, `netgraph.cfg` |

A composition at one layer **forward-declares** what it consumes from
the layer below. textX cross-file references are single-file, so every
prototype mentioned in a composition has a stub decl in the same file.
The real (rich) decl lives in the owning fragment.

## 2. Per-artefact spec

### 2.1 `<bus>/catalog.json`

**Produced by** `artheia import-dbc` (CAN) / `artheia import-fibex` (FlexRay).
**Source** raw DBC / FIBEX XML.
**Consumers** every downstream generator that needs signal layout.

Schema (FIBEX):

```json
{
  "bus": "mlbevo_gen2",
  "bus_kind": "flexray",
  "messages": {
    "EML_01": {
      "bus": "mlbevo_gen2",
      "bus_kind": "flexray",
      "pdu_id": "pdu_7009418",
      "pdu_type": "APPLICATION",
      "byte_length": 8,
      "fields": [
        {
          "name": "EML_BeschlX",
          "bit_position": 12,
          "bit_length": 11,
          "proto_type": "float",
          "is_signed": false,
          "motorola_byte_order": false,
          "values": [[2046, "Init"], [2047, "Fehler"]]
        }
      ],
      "frame_triggers": [ { ... per-occurrence wire-site info ... } ]
    }
  }
}
```

Schema (CAN):

```json
{
  "bus": "kcan",
  "bus_kind": "can",
  "messages": {
    "ACC_07": {
      "bus": "kcan",
      "bus_kind": "can",
      "can_id": 302,
      "extended_id": false,
      "dlc": 8,
      "fields": [ { ... same field shape as FIBEX ... } ]
    }
  }
}
```

**Key invariants:**
- Output is **PDU-centric**. For FIBEX, one entry per APPLICATION PDU
  (e.g. `EML_01`, not `FRAME_5_15_16`); same PDU on multiple frames
  produces one catalog entry with multiple `frame_triggers`.
- For DBC, CAN frames already *are* PDU-equivalent — one entry per
  frame.
- `fields[].values` is optional; present when the source defines a
  `VAL_` (DBC) or COMPU-METHOD TEXTTABLE (FIBEX) for that signal.
- `proto_type` is the wire-friendly scalar (`uint32`, `int32`, `float`,
  `bool`). Enum companions are emitted in `package.art` but the field
  type stays scalar so the codec layer doesn't need enum knowledge.

### 2.2 `<bus>/package.art`

**Produced by** the same `import-dbc` / `import-fibex` invocation as
the catalog.

One `message <PduName> { <field>... }` per PDU, plus one top-level
`enum <PduName>_<FieldName> { ... }` per signal that carries a value
table. Field types stay scalar; the enum is referenced in a `// enum:
...` trailing comment.

```artheia
package autosar.mlbevo_gen2_cmp_psp.system.mlbevo_gen2

enum ACC_07_ACC_Anhalten {
    kein_Anhalten_gewuenscht = 0
    Anhalten_gewuenscht = 1
}

message ACC_07 {
    uint32 ACC_07_CRC
    uint32 ACC_07_BZ
    uint32 ACC_Anhalten           // enum: ACC_07_ACC_Anhalten
    float  ACC_Anhalteweg         // enum: ACC_07_ACC_Anhalteweg
}
```

**Use this when:** you want to render the signal-level proto / nanopb
struct for a PDU. Both the catalog and the `.art` carry the same
information; the catalog is easier to parse, the `.art` is what gets
imported by vendor fragments via cross-file references.

### 2.3 `<bus>/netgraph.json`

**Produced by** `artheia gen-netgraph-partition --catalog X.json --out Y.json`.
**Consumers** any tool that needs to translate a symbolic PDU name into
a bus-level address.

Schema (FlexRay):

```json
{
  "bus": "mlbevo_gen2",
  "bus_kind": "flexray",
  "source_catalog": "autosar/mlbevo_gen2_cmp_psp/system/mlbevo_gen2/catalog.json",
  "routes": {
    "EML_01": {
      "byte_length": 8,
      "frame_triggers": [
        {
          "frame_name": "FRAME_5_15_16",
          "frame_byte_length": 34,
          "slot_id": 5,
          "cycle": 15,
          "cycle_repetition": 16,
          "channel": "channel_782614",
          "channel_idx": 0,
          "pdu_byte_offset": 0
        }
      ]
    }
  }
}
```

Schema (CAN):

```json
{
  "bus": "kcan",
  "bus_kind": "can",
  "source_catalog": "autosar/mlbevo_gen2_cmp_psp/system/kcan/catalog.json",
  "routes": {
    "ACC_07": {
      "can_id": 302,
      "extended_id": false,
      "dlc": 8
    }
  }
}
```

**Why a separate file (not just in catalog.json):**

The catalog answers "what's the shape of this PDU?". The netgraph
answers "where does this PDU ride on the wire?". They're orthogonal
concerns and consumers usually want one or the other. Splitting keeps
each focused; the catalog stays compact for proto/struct codegen, the
netgraph stays compact for runtime LUT generation.

### 2.4 `autosar/<psp>/system/system.art`

**Produced by** `artheia gen-autosar-system --catalog X.json [--catalog Y.json] --out Z.art --package P`.

One **mega-node per bus**, carrying a `sender` port per PDU. PDU
interfaces (`<PduName>_Iface`) are deduplicated across buses (PDUs
shared between CAN and FlexRay get one interface decl).

```artheia
package autosar.mlbevo_gen2_cmp_psp.system

interface senderReceiver EML_01_Iface { }
interface senderReceiver ACC_06_Iface { }
// ... 1441 interfaces total ...

node atomic Mlbevo_Gen2_Bus {
    tipc type=0x9f000000 instance=0   // synthetic — bus is wire, not TIPC
    ports {
        sender pdu_eml_01 provides EML_01_Iface
        sender pdu_acc_06 provides ACC_06_Iface
        // ... 1025 ports ...
    }
}

node atomic Kcan_Bus {
    tipc type=0x9f000001 instance=0
    ports {
        sender pdu_acc_07 provides ACC_07_Iface
        // ... 512 ports ...
    }
}
```

**Why a "bus" is modeled as a node:**

The Artheia grammar's `port` construct lives inside `node atomic`.
There is no top-level `port` declaration. To expose per-PDU sender
ports we wrap them in a node — even though a bus isn't really a TIPC
node. The synthetic TIPC address (`0x9F` prefix) advertises this.

**Naming:**
- Bus node: `<BusName>_Bus`, PascalCase per `_` token.
  `mlbevo_gen2` → `Mlbevo_Gen2_Bus`.
- Port: `pdu_<lower(PduName)>`. `BV2_Objekt_01` → `pdu_bv2_objekt_01`.
- Interface: `<PduName>_Iface`. `EML_01` → `EML_01_Iface`.

### 2.5 `gateway/system/package.art`

**Hand-written.** Lives in pero_theia, not a separate component repo.

Contains:
1. The `Status` clientServer interface + `StatusReport` message — the
   gateway's RPC.
2. A `Gateway` atomic node with a TIPC address and a `server status
   provides Status` port. This is the **real** gateway TIPC endpoint.
3. Forward-declarations of `Mlbevo_Gen2_Bus` and `Kcan_Bus` (empty
   port lists locally — the real shape lives in
   `autosar/<psp>/system/system.art`).
4. A `GatewayBridge` composition that prototypes both AUTOSAR buses
   alongside the Gateway TIPC node.

```artheia
package gateway.system

message StatusReport { ... }
interface clientServer Status { operation GetStatus() returns StatusReport }

node atomic Gateway {
    tipc type=0xa0010001 instance=0
    ports { server status provides Status }
}

// forward decls
node atomic Mlbevo_Gen2_Bus { tipc type=0x9f000000 instance=0 ports { } }
node atomic Kcan_Bus        { tipc type=0x9f000001 instance=0 ports { } }

composition GatewayBridge {
    prototype Mlbevo_Gen2_Bus mlbevo_gen2_bus
    prototype Kcan_Bus        kcan_bus
    prototype Gateway         gw
}
```

**The composition does not emit `connect` lines per PDU.** With 1500+
PDUs that would balloon the file with no extra information — the
routing is already in `netgraph.json`. The composition exists so
downstream tools can find the bus prototypes by walking the model.

### 2.6 `vendor/<v>/system/`

**Hand-authored (or generated from upstream specs like signals.md).**
Lives in a separate GitLab repo per vendor, checked into the workspace
via `.repo/local_manifests/`.

Layout:

```
vendor/<v>/system/
├── package.art                  # one senderReceiver interface per consumed PDU
├── components/
│   └── <node>.art               # node decl with receiver ports
└── system.art                   # composition of the vendor's nodes
```

Example (`vendor/odd_path_client/system/components/odd_path_monitor.art`):

```artheia
package vendor.odd_path_client.system.components
import vendor.odd_path_client.system.*

// forward decls of imported interfaces
interface senderReceiver EML_01_Iface { }
interface senderReceiver ACC_06_Iface { }
// ... per PDU ...

interface clientServer Status { }     // from gateway/system

node atomic OddPathMonitor {
    tipc type=0xc0010001 instance=0
    ports {
        receiver eml_01     requires EML_01_Iface
        receiver acc_06     requires ACC_06_Iface
        // ... one receiver per consumed PDU ...
        client   status_query requires Status
    }
    params {
        publish_period_ms : uint32 = 50
    }
}
```

A vendor app references AUTOSAR PDUs by interface (`EML_01_Iface`,
`ACC_06_Iface`), which by convention maps to PDU `<X>` — the `_Iface`
suffix is the join key for the codegen.

### 2.7 `platform/system/system.art`

**Hand-authored.** Lives in pero_theia.

The top-level composition. Forward-declares only the prototypes it
wires; the rest (FlexRay receivers etc.) stays in the imported
fragments.

Symlinks under `platform/system/` give downstream tools a single tree
to traverse:

```
platform/system/
├── system.art                                       # composition (this file)
├── autosar         -> ../../autosar/mlbevo_gen2_cmp_psp/system
├── gateway         -> ../../gateway/system
└── odd_path_client -> ../../vendor/odd_path_client/system
```

### 2.8 `platform/config/host_netgraph.json`

**Produced by** `artheia gen-host-netgraph --art F1.art [--art F2.art] --out H.json`.

Walks every `.art` passed in, finds `node atomic` decls that carry a
`tipc type=...` clause, and emits the **symbolic port → TIPC address**
LUT.

```json
{
  "sources": [
    "platform/system/system.art",
    "vendor/odd_path_client/system/components/odd_path_monitor.art"
  ],
  "nodes": {
    "Gateway": {
      "tipc_type": "0xa0010001",
      "tipc_instance": 0,
      "ports": {
        "status": { "kind": "server", "interface": "Status" }
      },
      "source": "platform/system/system.art"
    },
    "OddPathMonitor": {
      "tipc_type": "0xc0010001",
      "tipc_instance": 0,
      "ports": {
        "eml_01":       { "kind": "receiver", "interface": "EML_01_Iface" },
        "acc_06":       { "kind": "receiver", "interface": "ACC_06_Iface" },
        "status_query": { "kind": "client",   "interface": "Status" }
      },
      "source": "vendor/odd_path_client/system/components/odd_path_monitor.art"
    }
  }
}
```

**Conflict policy:** when the same node appears in multiple files
(forward-decl in `platform/system/system.art` + full decl in
`vendor/.../components/*.art`), the entry with the **richer** port
list wins. TIPC address mismatches are warned.

**Not included:** the AUTOSAR mega-nodes. Their synthetic TIPC
addresses are not real endpoints, and including them in the host LUT
would mislead the runtime.

### 2.9 `platform/config/netgraph.cfg`

**Hand-authored** (or trivially generated). The **index** of every
partition the platform composition depends on.

```json
{
  "platform_system": "platform/system/system.art",
  "autosar_partitions": [
    {
      "bus": "mlbevo_gen2",
      "bus_kind": "flexray",
      "system_art": "autosar/mlbevo_gen2_cmp_psp/system/system.art",
      "catalog":    "autosar/mlbevo_gen2_cmp_psp/system/mlbevo_gen2/catalog.json",
      "netgraph":   "autosar/mlbevo_gen2_cmp_psp/system/mlbevo_gen2/netgraph.json"
    },
    {
      "bus": "kcan",
      "bus_kind": "can",
      "system_art": "autosar/mlbevo_gen2_cmp_psp/system/system.art",
      "catalog":    "autosar/mlbevo_gen2_cmp_psp/system/kcan/catalog.json",
      "netgraph":   "autosar/mlbevo_gen2_cmp_psp/system/kcan/netgraph.json"
    }
  ],
  "host_netgraph": "platform/config/host_netgraph.json"
}
```

A code generator loads this file first, follows the paths into the
partitions it needs, joins by the keys described below.

## 3. Join keys: how the artefacts compose

Once a generator has loaded the artefacts via `netgraph.cfg`, the
joins it needs are:

```mermaid
flowchart LR
    classDef key fill:#fff2cc,stroke:#b58900
    iface[interface name<br/>EML_01_Iface]:::key --> pdu[PDU name<br/>EML_01]
    pdu --> shape[catalog.messages[pdu]<br/>fields, types, enums]
    pdu --> wire[netgraph.routes[pdu]<br/>slot/cycle/channel<br/>or can_id/dlc]
    port[symbolic port name<br/>odd_path_monitor.eml_01]:::key --> nodeRef[host_netgraph.nodes[NodeName].ports[PortName]<br/>tipc_type, interface]
    nodeRef --> iface
```

**Rules:**

1. **Interface name ↔ PDU name** is by string convention:
   `<PduName>_Iface` ↔ `<PduName>`. Strip the `_Iface` suffix.
2. **PDU shape lookup**: `catalog.messages[pdu_name]` gives fields +
   types + value tables.
3. **PDU wire lookup**: `netgraph.routes[pdu_name]` gives bus
   addressing.
4. **Symbolic-port → TIPC**: `host_netgraph.nodes[node_name].ports[port_name]`
   gives tipc_type + interface; from the interface, recover the PDU
   name via rule 1.

A PDU may appear in multiple `netgraph.routes` entries (FlexRay PDU
ridable on multiple frames). Code generators pick one per route — by
convention the first entry; an explicit override mechanism is a
follow-up.

## 4. End-to-end trace: a single PDU arriving at the app

Scenario: a frame carrying `EML_01` arrives on the FlexRay bus and
`OddPathMonitor` consumes its `EML_GeschwX` field.

```mermaid
sequenceDiagram
    participant Bus as FlexRay bus
    participant HW  as Hercules TMS570 capture
    participant CMP as ASAM-CMP encoder
    participant GW  as Gateway service (TIPC svc)
    participant PSP as libpsp_local.so<br/>(codec dispatch)
    participant App as OddPathMonitor

    Bus->>HW:  Frame at slot 5, cycle 15, ch A
    HW->>CMP:  Raw frame bytes
    CMP-->>GW: UDP packet (CMP wire format)
    GW->>PSP:  decode PDU EML_01<br/>(byte_offset 0, len 8)
    PSP-->>GW: decoded EML_01 nanopb struct
    Note over GW: lookup target via<br/>host_netgraph.json
    GW-->>App: TIPC publish to<br/>OddPathMonitor.eml_01<br/>(tipc_type 0xc0010001)
    App->>App: handle EML_01 callback;<br/>read .EML_GeschwX
```

**Step-by-step with the artefact each step consults:**

| Step | What | Artefact consulted |
| --- | --- | --- |
| 1 | Bus → Hercules capture | (none; hardware) |
| 2 | Hercules → ASAM-CMP encode | Hercules firmware (built from `gateway/firmware/`) |
| 3 | CMP UDP → Gateway service | `gateway/libs/pero_cmp_lnx/` (libcmpdecoder) |
| 4 | Service routes the incoming CMP packet to PDU `EML_01` | `mlbevo_gen2/netgraph.json` (reverse lookup: slot/cycle/channel → PDU name) |
| 5 | Decode the PDU bytes into the nanopb struct | `mlbevo_gen2/catalog.json` (field layout) and the PSP codec (built from the same source via `gen-platform-protos`) |
| 6 | Gateway looks up "who subscribes to `EML_01`?" | Vendor app's `eml_01` receiver port + `host_netgraph.json` (port → tipc_type) |
| 7 | Gateway publishes to TIPC `0xc0010001` | TIPC kernel transport |
| 8 | OddPathMonitor receives, dispatches to a callback bound to `eml_01` | App code generated from `vendor/odd_path_client/system/` |

The codegen's job is to produce the runtime LUT that makes step 6
fast. That LUT comes from joining `host_netgraph.json` against
the platform composition's `connect`s (or, for receiver-only flows,
against the receiver port's interface name).

## 5. Outbound (app → bus): inverse of the above

For an app that *sends* a signal onto the bus (e.g. a setpoint), the
flow inverts:

1. App: `send_signal_X(dst, data)` where `dst` is the symbolic port
   name and `data` is the proto message.
2. Generated code looks up `dst` in `host_netgraph.json` to find the
   gateway's TIPC address (`0xa0010001`) and the interface name.
3. TIPC packet → Gateway service.
4. Gateway encodes via PSP codec (`catalog.json` for layout).
5. Gateway looks up `EML_01` in `netgraph.json` to get the wire
   address.
6. Sends CMP UDP → Hercules → bus.

`odd_path_client` today has no sender ports — its consumption is
read-only. Other vendors (or future versions of odd_path) will follow
this path.

## 6. Recipes

### 6.1 Add a new vendor app

1. Create a GitLab repo `PG50/<vendor>.git`.
2. `mkdir vendor/<vendor>/` (locally) and build the layout:
   ```
   vendor/<vendor>/
   ├── README.md
   └── system/
       ├── package.art           # senderReceiver interfaces per consumed PDU
       ├── components/<node>.art # node decl with ports + params
       └── system.art            # composition
   ```
3. Push to GitLab.
4. Add `.repo/local_manifests/vendor_<vendor>.xml` (template:
   `docs/local_manifests/vendor_tornado.xml`).
5. `repo sync vendor/<vendor>` to register the checkout.
6. Add the app to `platform/system/system.art` — forward-decl the node
   and add its prototype to the `Platform` composition.
7. Regenerate `platform/config/host_netgraph.json`:
   ```sh
   artheia gen-host-netgraph \
       --art platform/system/system.art \
       --art vendor/<vendor>/system/components/<node>.art \
       --out platform/config/host_netgraph.json
   ```

### 6.2 Regenerate AUTOSAR layers after a FIBEX/DBC update

```sh
PSP=autosar/mlbevo_gen2_cmp_psp

# Re-import — produces catalog.json + package.art per bus
artheia import-fibex --fibex $PSP/config/MLBevo_*.xml \
    --bus mlbevo_gen2 --out $PSP/system/mlbevo_gen2 \
    --package autosar.mlbevo_gen2_cmp_psp.system --no-validate

artheia import-dbc --dbc $PSP/config/dbc/MLBevo_Gen2_*KCAN_*.dbc \
    --bus kcan --out $PSP/system/kcan \
    --package autosar.mlbevo_gen2_cmp_psp.system

# Re-derive netgraph partitions
artheia gen-netgraph-partition --catalog $PSP/system/mlbevo_gen2/catalog.json \
    --out $PSP/system/mlbevo_gen2/netgraph.json
artheia gen-netgraph-partition --catalog $PSP/system/kcan/catalog.json \
    --out $PSP/system/kcan/netgraph.json

# Re-emit the mega-node system.art
artheia gen-autosar-system \
    --catalog $PSP/system/kcan/catalog.json \
    --catalog $PSP/system/mlbevo_gen2/catalog.json \
    --out $PSP/system/system.art \
    --package autosar.mlbevo_gen2_cmp_psp.system
```

### 6.3 Run the codegen after changing the platform composition

```sh
artheia gen-host-netgraph \
    --art platform/system/system.art \
    --art vendor/odd_path_client/system/components/odd_path_monitor.art \
    --out platform/config/host_netgraph.json
```

## 7. Known limitations

**textX cross-file references are single-file.** Every prototype
mentioned in a composition has a forward-decl stub in the same file.
The richer real decl lives in the imported fragment. Generators that
walk the model must follow imports manually.

**Synthetic TIPC on bus mega-nodes.** The `0x9F0000xx` addresses on
`Mlbevo_Gen2_Bus` / `Kcan_Bus` are placeholders to satisfy the grammar
— a bus is not a TIPC endpoint. The host netgraph generator
intentionally drops these from the LUT (it doesn't see the AUTOSAR
mega-nodes when called against the platform composition).

**Per-PDU `connect` lines.** The platform / gateway compositions
intentionally do *not* enumerate `connect <bus>.pdu_X to gw.X` for
every PDU. With 1500+ PDUs that would balloon the `.art` for no
extra information — routing lives in the netgraph LUTs.

**Same-name PDUs across buses share an interface.** A PDU named
`EML_01` on both CAN and FlexRay produces one `EML_01_Iface` decl in
the AUTOSAR `system.art`. The split is by `frame_triggers` /
`can_id` in the catalog. If a codegen needs to disambiguate, key on
the catalog's `bus` field per message.

**`_StdA` companion signals are dropped at import time.** The DBC /
FIBEX often carry per-signal standard-deviation companions; the
importer skips them by convention (no consumer on the host).

## 8. Commands cheat-sheet

```sh
# Import:
artheia import-dbc   --dbc X.dbc   --bus B --out D [--csv F] [--package P]
artheia import-fibex --fibex X.xml --bus B --out D [--csv F] [--package P] [--no-validate]

# AUTOSAR layer:
artheia gen-netgraph-partition --catalog X.json --out Y.json
artheia gen-autosar-system     --catalog X.json [--catalog Y.json ...] --out Z.art --package P

# Platform / host layer:
artheia gen-host-netgraph --art F1.art [--art F2.art ...] --out H.json

# Per-app codegen (existing, run from the gateway service tree):
artheia gen-signal-filter --vendor-root D --out C.csv
artheia gen-app-dispatch  --psp-root D --csv C.csv --out G
artheia gen-platform-protos --fibex X.xml --dbc Y.dbc:bus --out-src S --out-proto P --all-signals

# Inspect:
artheia parse <file.art>
artheia signal-filter --config <psp-config-dir>   # interactive REPL
```

## 9. Where to start when writing a new codegen

1. Load `platform/config/netgraph.cfg`; resolve relative paths against
   the workspace root.
2. Load every partition listed (catalog, netgraph, system.art) and
   `host_netgraph.json`.
3. Parse `platform/system/system.art` to discover the
   composition's prototypes + connect lines (the runtime wiring graph).
4. For each port the composition uses:
   - Find its `node.port` entry in `host_netgraph.json` to get TIPC
     coords + interface name.
   - Strip `_Iface` from the interface name → PDU name (skip if the
     interface is a clientServer one, like `Status`).
   - Look up the PDU in the relevant bus's `catalog.json` and
     `netgraph.json`.
5. Emit your output (runtime LUT, signal_filter.csv, header, ...).

Keep the generator restricted to the artefacts above. Do **not**
re-parse FIBEX or DBC directly — that's the importer's job, and
catalog.json is its contract.
