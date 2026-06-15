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

## Artheia

All artheia-side docs live under [`artheia/`](artheia/):

- [Overview](artheia/README.md) — what artheia is and how to start.
- [Manual](artheia/manual.md) — comprehensive user manual.
- [Manifest module reference](artheia/manifest.md) — per-file map of
  the manifest dataclass hierarchy.
- [Manifest DSL reference](artheia/manifest-dsl.md) — `Layer.squash` +
  `SoftwareSpecification` + set transforms (`Append` / `Remove`) +
  value markers (`Undefined` / `Default` / `Defer`). The mosaic-style
  structured DSL for composing vendor / platform / vehicle manifest
  layers.
- [Tutorial: bootstrap a rig from system.art](artheia/gen-rig.md) —
  end-to-end walkthrough of `artheia gen-rig`, the generator that
  emits a starter `rig.py` from a top-level artheia composition.
- [Export reference](artheia/export.md) — what the artheia codegen
  emits and how to consume it downstream.
- [Editor integrations](artheia/editors.md) — `.art` syntax + LSP for
  VS Code and Emacs (`contrib/editors/`).
- [Shell completion](artheia/completion.md) — tab-complete for
  `artheia` and the workspace launcher `theia`.

## Workspace launcher

- `theia.py` at the workspace root — single dispatcher for artheia
  + bazel + repo + supervisor bringup. See `theia --help` and
  [shell completion setup](artheia/completion.md).
