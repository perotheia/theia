# The `.art` language

Reference for the artheia DSL. Grammar source of truth:
`artheia/artheia/grammar/artheia.tx` (textX). This page summarizes it with
real examples from the tree — when in doubt, read the `.tx`.

## File shape

```scala
Model:
    ('package' name=QualifiedName)?
    imports*=Import
    elements*=TopLevelElement
```

A file is an optional `package` line, then `import` lines, then top-level
elements. **Imports must precede every element** — the grammar is
`package? imports* elements*`, so an `import` after any declaration is a
syntax error.

A package is split across two sibling files that the loader **merges** into
one model: `package.art` (the schema — messages, enums, interfaces, nodes)
and `component.art` (the wiring — compositions, clusters). They must declare
the same `package`. The merger hoists `import` lines from **both** files to
the top, so either file may carry imports.

File-name resolution priority for `import X.Y.*`: the resolver looks under
the directory mapped from the FQN for `system.art` → `cluster.art` →
`package.art` → `component.art`.

```scala
package system.services.exec
import system.supervisor.*           // imports first, always
message SupervisionEvent { }         // then forward declarations
```

## Top-level elements

`message` · `enum` · `interface` · `node` · `composition` · `cluster` ·
`bus` · `gateway_route`.

### message / enum (proto3-equivalent)

```scala
message LogRecord {
    string context
    uint32 level
    uint64 ts_ns
    string text
}

enum FgState { IDLE = 0  STARTING = 1  RUNNING = 2  STOPPING = 3 }
```

Scalar field types: `int32 int64 sint32 sint64 uint32 uint64 fixed32
fixed64 sfixed32 sfixed64 float double bool string bytes`. A field may also
reference another `message`/`enum` by name, and may be `repeated`.

### interface — two flavors

```scala
// pub/sub, fire-and-forget; one message type per `data`:
interface senderReceiver LogStream {
    data LogRecord record
}

// RPC, request/reply; operations with in/out/inout params:
interface clientServer ExecCtl {
    operation StartGroup(in r:StartGroupRequest) returns ExecEmpty
    operation StopGroup(in r:StopGroupRequest)   returns ExecEmpty
}
```

### node — the thread

```scala
NodeDecl:
    'node' kind=NodeKind name=ID ('extends' base=[NodeDecl|FQN])? '{'
        tipc=TipcAddress
        (kick_off?='kick_off')?
        (requires_timers?='requires_timers')?
        ('reporting' '=' reporting=BoolLit)?
        ('fallthrough' '=' fallthrough=BoolLit)?
        ('tag' '=' tag=STRING)?
        ('config' config=[MessageDecl|FQN])?
        ('ports' '{' ports*=PortDecl '}')?
        ('params' '{' params*=NodeParam '}')?
        ('statem' '{' statem=StateMBody '}')?
    '}'
```

- **`kind`** is `atomic` (a `GenServer`, or `GenStateM` if a `statem` block
  is present) or `runnable` (a `GenRunnable` free worker — no `statem`).
- **`tipc type=0x… instance=0`** is mandatory on every node (the network
  address; one per node). Even a forward-decl stub needs one.
- **`tag = "LOG"`** — short context id for log lines.
- **`reporting = true|false`** — whether the supervisor can address this
  node for heartbeat / trace-config push (defaults true).
- **`fallthrough = true`** — route unmatched TIPC frames to `handle_info`
  instead of aborting. **Test probes only**; production nodes must not set it.
- **`extends Base`** — prototype inheritance: the derived node absorbs the
  base's ports/params/statem/config wholesale unless it re-declares them
  (no field-level merge). Most "derived" nodes only override `tipc`.
- **`config Msg`** — the node's etcd-backed config message type.

```scala
node atomic TraceCollector {
    tipc type=0x80010013 instance=0
    tag = "LOG"
    ports {
        server   ctl_supdbg     provides TraceControl
        sender   stream_out     provides TraceRecordStream
        receiver in_records     requires TraceRecordSubmit
        client   to_supervisor  requires SupervisorControlIf
        sender   to_log         provides LogStream  best_effort
    }
}
```

#### ports

Four port kinds. `provides`/`requires` must match the interface flavor:

| port | interface | direction |
| --- | --- | --- |
| `server  X provides Iface` | clientServer | this node answers RPCs |
| `client  X requires Iface` | clientServer | this node calls RPCs |
| `sender  X provides Iface [reliable\|best_effort]` | senderReceiver | this node publishes |
| `receiver X requires Iface [reliable\|best_effort]` | senderReceiver | this node subscribes |

Reliability defaults to `reliable`; `best_effort` is fire-and-forget (used
for log/trace fan-in where a drop must never block the app).

#### params (ROS2-style, etcd-backed)

```scala
params {
    publish_period_ms : uint32 = 10
    enabled           : bool   = true
    source_name       : string = "front-axle"
}
```

#### statem (gen_statem FSM)

```scala
statem {
    states [ Idle, Running, Stopping ]
    initial Idle
    data SmContext
    on Idle:
        event StartReq → Running
    on Running:
        event StopReq → Stopping
        timeout → halt
}
```

Transition arrows are the Unicode `→` (U+2192), not `->`.

### composition — the process

```scala
composition VehicleSystem {
    prototype SpeedPublisher    speed_pub
    prototype TorqueController  torque_ctrl
    connect speed_pub.out          to torque_ctrl.speed_in
    connect torque_ctrl.torque_out to actuator.torque_in
}
```

- **`prototype NodeType name`** instantiates a node; optional
  `on process P` is an affinity hint.
- **`composition CompType name`** includes another composition.
- **`connect a.port to b.port`** wires ports. A port ref is
  `<prototype>.<port>`; prototype names are globally unique in the model, so
  no member-prefix qualification is needed.

### cluster — the distribution bundle

```scala
cluster Services {
    composition Com   com
    composition Log   log
    composition Per   per
    composition Sm    sm
    composition Ucm   ucm
    composition Shwa  shwa
}
```

Each `composition T name` member is one installable package keyed by
`name`. `connect` lines inside a cluster are **inter-process** TIPC (vs the
in-process wiring inside a composition).

### bus / gateway_route (AUTOSAR PSP)

```scala
bus kcan kind = can
gateway_route SpeedPublisher {
    can id=0x42 bus=kcan dlc=8
    direction = in
}
```

`gateway_route` maps a node's PDU to a CAN id / FlexRay slot, with
`direction = in|out`. These appear in the generated AUTOSAR mega-node
specs, not in hand-written FC specs.

## Forward declarations

Cross-file references resolve through the **import + forward-decl** pattern:
declare an empty stub locally so the file parses standalone, and the
resolver materializes the real definition from the imported package.

```scala
// system/system.art
package system
import system.services.*
import system.supervisor.*

cluster Services         { }   // stub → real def in system.services
composition Supervisor   { }   // stub → real def in system.supervisor
cluster Platform {
    composition Supervisor sup
}
```

A stub node still needs a `tipc` (grammar requires it); mirror the real
one. A stub message is just `message Name { }`. This is exactly how
`services/nop/exec/` references `system.supervisor`'s `Supervisor` node and
`SupervisionEvent` message — see that FC and `system/services/cluster.art`
for worked examples.

## Validate

```sh
PATH="$PWD/.venv/bin:$PATH" artheia parse <file.art>
```

Resolves imports recursively and prints the merged tree, or exits non-zero
on the first error. Common errors: an `import` placed after a declaration;
a `node` missing its `tipc`; `→` typed as `->`; a `provides`/`requires`
pointing at the wrong interface flavor. The LSP (`artheia-lsp --stdio`,
VS Code extension under `artheia/vscode-extension/`) gives the same
diagnostics live, plus go-to-definition across the forward-decl boundary.
