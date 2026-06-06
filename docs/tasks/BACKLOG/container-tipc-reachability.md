# Container TIPC reachability — tdb can't reach a containerized supervisor

`tdb` drives the supervisor over **TIPC** (via `artheia.probe` → the
`SupervisorCtl` node, per the `clients-via-art-probe` design). The
`docker compose` stack puts each supervisor on a **docker bridge network**
(`theia_net`, 172.30.0.0/24). TIPC does not traverse a docker bridge out of the
box, so host-side `tdb ps` (or a GUI) cannot reach a containerized supervisor's
TIPC namespace — even when the container is healthy. Found 2026-06-06 verifying
`tdb ps` against compose.

This is the "no TIPC network isolation yet" gap noted across the project.

## Why it matters

The two deploy paths diverge precisely here:

| path | supervisor | tdb | TIPC namespace | `tdb ps` works? |
| --- | --- | --- | --- | --- |
| `theia install` (local) | host process | host process | shared (host) | yes |
| `docker compose` | container | host | container ≠ host | **no** |

So the compose stack can be perfectly healthy and still un-observable from the
host's tdb. The deploy mechanism is not the blocker — the transport boundary is.

## Options (pick when this is picked up)

1. **`--network=host`** for the compose services. Simplest: the container shares
   the host network + TIPC namespace, so host tdb reaches the supervisor like a
   local one. Loses the bridge's per-container address isolation and the
   static-subnet `machines.json` addressing; fine for a dev bring-up.
2. **TIPC inter-node links across the bridge** — `tipc-config`/`tipc link`
   configured so the container's TIPC node and the host (or the other
   container) form a cluster over the bridge interface. Closest to the real
   multi-ECU topology; most setup.
3. **Run tdb inside the container** (`docker exec theia-central tdb ps`) — tdb
   shares the container's TIPC namespace. Works today with zero transport
   plumbing; just not host-driven. Good enough to *verify a compose bring-up*
   even before 1/2 land.
4. **Bridge tdb over the gRPC edge** — if com's gRPC bridge (currently hidden)
   is the supervisor↔external transport, tdb could speak gRPC to `:7700`/`:7701`
   instead of TIPC. Depends on the com gRPC fix (separate next-session item) and
   on tdb gaining a gRPC transport (today it's probe/TIPC only — the probe
   design makes transport swappable, so this is a probe-transport addition).

## Recommendation

Option 3 (`docker exec … tdb ps`) for immediate verification once the rig
packages the FC binaries (see `rig-image-packages-only-supervisor.md`); option 1
(`--network=host`) as the dev-loop default; options 2/4 for the real topology.

## Status

Not started. Independent of the rig-packaging gap, but BOTH must land before a
host-driven `docker compose` + `tdb ps` works end-to-end. The
`clients-via-art-probe` + swappable-transport design means option 4 is the
clean long-term answer (change runtime + probe, tdb keeps working).
