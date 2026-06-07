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

**Option 1 DONE 2026-06-07** — compose switched to `network_mode: host` on both
services (dropped the bridge network / aliases / port maps; compute overrides
THEIA_COM_LISTEN→7701 against a future com :7700 collision). Verified: with
central up, host-side `tdb ps` renders the full supervision tree from the
containerized supervisor over the shared host TIPC namespace (sm/log/per/ucm/
shwa + nodes, with tipc addresses). This is the dev default.

Surfaced a real adjacent fix: the docker IMAGE had a STALE baked
run-supervisor.sh (the old `supervisor run <json> --root-dir` argv form) — the
gen-app supervisor takes NO argv and reads THEIA_SUPERVISOR_MANIFEST, so it fell
back to `supervisor_tree.json` and crash-looped. Rebuilding `theia-base` (which
COPYs run-supervisor.sh) then `theia-central` fixed it. **Gotcha for next time:
`docker compose build` does NOT rebuild `theia-base` — rebuild it explicitly
(`docker build -f deploy/Dockerfile.base -t theia-base:latest deploy/`) whenever
run-supervisor.sh changes.**

Options 2 (TIPC links across a bridge) and 4 (tdb over gRPC) remain the answer
for the REAL multi-ECU topology, where containers must NOT share one host TIPC
namespace. Host mode is dev-only.

Still-open adjacent gaps observed during this verification (tracked elsewhere):
- `per` fails to exec in-container: `libetcd-cpp-api.so` not found — per links
  libetcd, and the etcd lib mount was removed when etcd was cut from compose.
  per is the etcd client (not the supervisor); restore JUST that lib mount.
- p1/p2/p3 (compute-bound) still listed in the CentralRig executor tree →
  execvp failures on central (see rig-image-packages-only-supervisor.md, the
  executor-tree-slicing gap).
