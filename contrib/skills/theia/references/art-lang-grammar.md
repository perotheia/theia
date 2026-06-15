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
    extern?='extern'
    'node' kind=NodeKind name=ID ('prototype' base=[NodeDecl|FQN])? '{'
        (tipc=TipcAddress)?
        (requires_timers?='requires_timers')?
        ('reporting' '=' reporting=BoolLit)?
        ('tag' '=' tag=STRING)?
        ('config' config=[MessageDecl|FQN])?
        ('ports' '{' ports*=PortDecl '}')?
        ('params' '{' params*=NodeParam '}')?
        ('statem' '{' statem=StateMBody '}')?
    '}'
```

- **`kind`** is `atomic` (a `GenServer`, or `GenStateM` if a `statem` block
  is present) or `runnable` (a `GenRunnable` free worker — no `statem`).
- **`extern`** marks a FORWARD declaration — the symbol exists, defined
  elsewhere (another package, resolved by the import-following scope
  provider). An `extern` node MUST have an empty body (no `tipc`, no ports);
  it's the explicit replacement for the old empty-body-is-a-stub heuristic.
- **`tipc type=0x… instance=0`** — the network address. Grammar-optional but
  loader-REQUIRED for a real (non-`extern`) node, UNLESS a `prototype <Base>`
  supplies it via inheritance. An `extern` node carries none.
- **`requires_timers`** — the node uses `process_timers()` (send_after /
  cancel_timer). Always-on in practice; the flag lets `main` publish the
  process `TimerService`.
- **`tag = "LOG"`** — short context id for log lines (defaults to the node
  name when omitted).
- **`reporting = true|false`** — whether the supervisor watchdogs this node
  and can push heartbeat / trace / log-level config to it (defaults true).
- **`prototype Base`** — attribute-REPLACEMENT inheritance (deliberately NOT
  `extends`): the derived node inherits the base's ports / statem / config /
  params / requires_timers unless it re-declares them (no field-level merge).
  The common case is overriding only `tipc`:
  `node atomic FooZonal prototype Foo { tipc type=0x… instance=0 }`.
- **`config Msg`** — the node's etcd-backed config message type.

There is no `kick_off` (retired — a node's post-construction startup work is
the OTP `init(State&)` callback the runtime invokes on the node thread) and no
`fallthrough` (retired — an unrouted inbound frame is dropped with a CRITICAL
log in `TipcMux`; cross-node traffic is exclusively typed `cast`/`call`, so a
test probe declares a typed receiver port per message instead). Tracing has no
annotation: every node is runtime-traceable, flipped by the supervisor.

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

## Forward declarations (`extern`)

Cross-file references resolve through the **import + `extern` forward-decl**
pattern: declare the symbol locally with the `extern` keyword and an EMPTY
body so the file parses standalone, and the import-following scope provider
materializes the real definition from the imported package.

`extern` is the explicit replacement for the old empty-body-is-a-stub
heuristic — an empty body is no longer magic. Say `extern` when you mean a
forward declaration. It prefixes a node, composition, cluster, or interface
(a message is never `extern` — `message Name { }` is its own forward decl).

```scala
// system/system.art
package system
import system.services.*
import system.supervisor.*

extern cluster Services { }            // real def in system.services
extern composition Supervisor { }      // real def in system.supervisor
cluster Platform {
    composition Supervisor sup
}
```

The empty `{ }` is still required — the keyword precedes the name and the
braces stay (`extern node atomic FlexRayIngress { }`,
`extern interface senderReceiver EML_01_Iface { }`).

Rules the loader enforces:

- An `extern` decl MUST have an empty body — no `tipc` / ports on a node, no
  members on a composition/cluster/interface. (Contrast the old rule, which
  made you mirror the real node's `tipc` on a stub; an `extern` node carries
  none.)
- A non-`extern` `node` is loader-REQUIRED to have a `tipc`, unless a
  `prototype <Base>` supplies it.

A node may also be specialized rather than forward-declared:
`node atomic FooZonal prototype Foo { tipc … }` inherits Foo's body and
overrides only what it re-declares (see `prototype` above).

## Validate

```sh
PATH="$PWD/.venv/bin:$PATH" artheia parse <file.art>
```

Resolves imports recursively and prints the merged tree, or exits non-zero
on the first error. Common errors: an `import` placed after a declaration;
a non-`extern` `node` missing its `tipc`; a non-empty `extern` body; `→`
typed as `->`; a `provides`/`requires` pointing at the wrong interface
flavor. The LSP (`artheia-lsp --stdio`; editor clients under
`contrib/editors/{vscode,emacs}/`) gives the same diagnostics live, plus
go-to-definition across the forward-decl boundary.
