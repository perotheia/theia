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
