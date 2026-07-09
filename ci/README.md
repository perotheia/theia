# ci/ — the end-user-flow harness

Reproducible verification of the **whole user story**, exactly as a user runs it
— locally on any dev box and on every push to main. This is the harness that
replaces hand-rerunning workspaces before a release.

```sh
./ci/run.sh              # everything (s1..s5)
./ci/run.sh s4 s5        # a subset
NO_LIVE=1 ./ci/run.sh    # toolchain only (no TIPC / no sudo — e.g. a container)
```

## What it covers

| | scenario | flow | runtime assertion (Robot, ci/test/) |
|---|---|---|---|
| s1 | ws-bare | `theia init --kind ws` → placeholder app → gen-app fc → bazel → manifest/install/start | node bound + answers `theia call`; **enabled-override regression** (a `deploy/config` param must reach the node); empty-cluster gen-app refuses; `.bazelversion`/`-c opt` pins |
| s2 | ws-demo | init + graft the Demo3Way seed (4 compositions, statem, connects, config) | 4 processes up; counter accumulated the driver's increments (== 50) |
| s3 | ws-services | `--with-services` → full FC tree manifests + builds | service tree up under one supervisor (per/nm held: no etcd/caps in CI) |
| s4 | pkg | `theia init --kind package` + sensor seed → package gen + tester gen → bazel | the scaffold's own robot probe; **params-alias regression** (both identity keys staged) |
| s5 | pkg-consume | sensor + filter package repos consumed by a fresh ws as bazel modules; cross-package `connect` | **data flows** producer→consumer (`GetStats.received` grows) |

Together the scenarios exercise every `theia init` kind/flag and every
`gen-app` mode a user touches (`--kind fc` per-composition, `--kind package`,
statem + atomic + timer nodes, sender/receiver/server ports, params, imported
packages, module-qualified labels).

## The contract (why this doesn't rot like demo/ did)

1. **Fresh scaffold every run.** Workspaces are generated under `ci/.work/`
   (gitignored) via the real entry points (`theia init`, `artheia gen-app`,
   bazel against `@pero_theia`). Nothing generated is committed, so nothing
   drifts when the scaffold or codegen changes — the harness follows.
2. **Seeds are USER-side code.** `ci/seeds/` + `ci/demo/` hold only what a user
   would write: `.art` sources and write-once impl bodies, grafted onto the
   scaffold **before** gen-app (which proves the write-once skip). Neutral names
   (sensor/filter/pilot/pipeline) — never a real downstream package's.
3. **Framework code must never import anything under `ci/`.** The dependency
   arrow points one way: ci/ consumes the framework through its public surface
   only. A concrete example leaking into generic codegen is a bug (see the
   v2v-comment purge, 2026-07-09).
4. **Regressions become scenarios.** Field bugs get a permanent assertion here
   (the identity seams, the params alias, the empty-cluster refusal already
   are). If you fixed it by hand once, encode it.

## Runtime phase

`theia start`/probes need the TIPC kernel module + sudo (supervisor setcap).
The harness auto-detects (`modprobe tipc`); without it, the toolchain phases
still run and hard-fail, and the live phase is skipped (`NO_LIVE=1` to force).
Scenarios run sequentially and `theia stop` between phases; the supervisor's
own 0x80020001 collision guard fails fast if another stack holds the slice.

## CI

`.github/workflows/selftest.yml` job `user-flow` runs `./ci/run.sh` on every
push to main — the same script, same seeds, same assertions as your dev box.

## Adding a scenario

Add a seed dir (user-side files only), an `sN()` function in `run.sh` (scaffold
→ graft → gen → build → manifest/install → live), and a Robot suite in
`ci/test/` for the runtime assertion. Wire `sN` into the default list.
