# Services in Artheia — DONE 2026-05-23

ref: docs/autosar/adaptive.md docs/autosar/services

## Original interpretation

```
services - Platform components(FC).
   ├── com - gRPC proxy only, aim for identity management. basically self signed certificate for grpc
   ├── core - pure conceptual. add core/include fit ar header. no system.art
   ├── crypto - empty
   ├── diag - forward DTS from classical autosar on demand (skip for now)
   ├── exec - implemented in supervisor . empty
   ├── fw - fw/config/rules.conf - iptables rules. no system.art
   ├── idsm - empty
   ├── log - Log and Trace. log part implemented by syslog, trace - trace service we have.
   ├── nm - external by linux. empty
   ├── osi - Operating System Interface (empty)
   ├── per - etcd proxy, use etcd-cli-c++ lib. etcd stores Node parameters from
            component.art, platform/vehicle/cluster parameters (tbd), always versioned.
            On migration writes both keys if both exist:
              /theia/v1/state/<m>/health and /theia/v2/state/<m>/health
            with mapping between old and new keys when not one-to-one.
   ├── phm Platform Health Management - empty. supervisor takes info from /proc fs
   ├── rds Raw Data Stream - empty. We will use it for blob messages in future
   ├── shwa Safe Hardware Accelerator - forward data from nvidia-smi on compute node.
   ├── sm - Node. run gen_statem inform others on state change. important
   ├── tsync - system ntpd. empty
   ├── ucm - Update and Configuration Management - OTA - important - forwards info from puppet agent.
   └── vucm - Vehicle Update and Configuration Management - hardcore update A/B partition - not implementing.
```

## What landed

Real FCs (interfaces + cross-cutting ports wired in `services/system/<fc>/package.art`):

- **log** — owner of `LogStream` (senderReceiver, best_effort fan-in).
  Every real FC has `sender to_log provides LogStream best_effort`;
  LogDaemon has `receiver log_in requires LogStream best_effort`.
- **per** — owner of `PersistencyIf` (clientServer Get/Put). Every real
  FC has `client to_per requires PersistencyIf`.
- **sm** — owner of `SmStateStream` (senderReceiver broadcast of
  `SmState` transitions) and `StateMgmtCtl` (clientServer for mode
  requests). Ucm subscribes; others can add `receiver from_sm` as
  they appear.
- **com** — exposes `ComBridge` server to external gRPC peers (GUI /
  admin console). Internally a client of per.
- **ucm** — `UpdateCtl` server for the fleet manager (via com).
  Subscribes to SmStateStream so it can pause updates when not in
  RUNNING. Persists progress via per.
- **shwa** — `AccelTelemetry` senderReceiver publisher. Reads
  `nvidia-smi`, persists samples via per.

Empty / not implemented (README.md + minimal `package.art` keeping
only the TIPC type reservation):

  core, crypto, diag, exec, fw, idsm, nm, osi, phm, rds, tsync, vucm.

## Cross-package interface references

The grammar resolves interfaces inside a single `.art` file, not across
packages. For now each consumer forward-declares the foreign interface
locally with an empty body — e.g. `interface senderReceiver LogStream { }`
in com/per/sm/ucm/shwa. The body is authoritative in the OWNER package
(`system.services.log`, `…per`, `…sm`); the local empty decl is a
placeholder until artheia cross-file imports land (see
`TODO/system-art-aggregation.md`).

## Smoke

- `artheia parse` on every services/system/*/package.art → all OK.
- `artheia gen-proto services/system/log/package.art` → emits LogRecord.proto.
- `artheia gen-proto platform/supervisor/system/package.art` → unchanged.
- `artheia generate-manifest apps.manifest.rig` → unchanged supervision tree.
