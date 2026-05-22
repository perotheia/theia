# Theia Docs

Documentation for the Porsche PERO CMP gateway workspace.

## Architecture

- [System architecture](architecture/ARCHITECTURE.md) — workflow + artefact spec
  from FIBEX/DBC through netgraph and system.art to the gateway and the
  vendor app. The spec code generators read from.

## Gateway

- [Gateway overview](gateway/GATEWAY.md)
- [Gateway plan](gateway/GATEWAY_PLAN.md)
- [Layers](gateway/LAYERS.md)
- [Time sync](gateway/TIMESYNC.md)
- [Workflow](gateway/WORKFLOW.md)

## Applications

- [Application generation & lifecycle](application.md) — `artheia gen-app`,
  the three-slice scaffold, `LifecycleInterface`, rx loop, handlers, build,
  and pcap-replay end-to-end run.

## Runtime

- [platform/runtime — the actor runtime](runtime.md) — actor nodes,
  mailboxes, call/cast/send_request semantics, local + cross-process
  refs, timers, runtime tracing, and generation from `.art`. Includes
  sequence diagrams and implementation notes. The runtime under
  `platform/runtime/` that newer applications build on.

## Manifest DSL

- [Manifest DSL reference](manifest-dsl.md) — `Layer.squash` +
  `SoftwareSpecification` + set transforms (`Append` / `Remove`) +
  value markers (`Undefined` / `Default` / `Defer`). The
  mosaic-style structured DSL for composing vendor / platform /
  vehicle manifest layers.
- [Tutorial: bootstrap a rig from system.art](tutorials/gen-rig.md)
  — end-to-end walkthrough of `artheia gen-rig`, the generator
  that emits a starter `rig.py` from a top-level artheia
  composition.
