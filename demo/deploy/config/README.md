# Config overrides — applied at DEPLOY time, per rig

This directory holds config overrides deep-merged onto the machine-generic
defaults emitted by `artheia gen-params`. **The overrides are NOT baked into the
built artifact** — `dist/manifest/<machine>/config/<fc>.json` is the pure
`gen-params` default, a function of (arch, os, version) only. Overrides are
applied at *deploy* time by `theia orchestrate <target>` (colony's
`tasks/config-override.yml`), against the running device.

This keeps the artifact **build-once**: the same `dist/manifest/<machine>/` is
reused by every rig that runs that machine slice; what differs per rig is the
deploy-time config merge.

## Two override layers (both applied at deploy time, in order)

For each FC, on top of the machine-generic default:

1. **per-machine** `deploy/config/<machine>/<fc>.json` — the MACHINE-shared
   layer. Keyed by the artheia `machine` (`central`, `compute`), resolved from
   the deploy target's slice. e.g. `central/tsync.json` = GPS-disciplined PTP
   grandmaster; `compute/tsync.json` = PTP slave. Shared by every rig on that
   machine.
2. **per-target** `deploy/config/<target>/<fc>.json` — the per-RIG layer, ON
   TOP. Keyed by the deploy TARGET name (the rig in
   `deploy/registry/<target>.yml`), NOT the machine — so two boxes running the
   same `central` slice can differ. e.g. `rpi4` runs the `central` slice but has
   RTK GPS instead of a PTP NIC, so `deploy/config/rpi4/tsync.json` disables
   ptp4l/phc2sys/gpsd (and `rpi4/nm.json` rides WiFi).

The merge precedence composes naturally: a rig gets its machine's profile, then
its own rig-specific tweaks win. (e.g. rpi4 inherits central's `ptp4l.args` but
sets `ptp4l.enabled = false`.)

## Layout

```
deploy/config/<machine-or-target>/<fc>.json
```

- `<machine>` — an artheia machine (`central`, `compute`, …).
- `<target>` — a deploy rig name from `deploy/registry/<target>.yml`.
- `<fc>` — the function-cluster (service) name, e.g. `tsync`.

## How it works

The machine-generic default is the static per-node `params` from the `.art`,
structured by node section:

```json
{"package": "...",
 "nodes": {"gpsd":  {"enabled": false, "args": "-N -n /dev/ttyGPS0"},
           "ptp4l": {"enabled": true,  "args": "-i ptpIf0 -s -m -q"},
           ...}}
```

At deploy time, colony deep-merges `deploy/config/<machine>/<fc>.json` then
`deploy/config/<target>/<fc>.json` onto it (ansible `combine(recursive=True)`):

- Each override is **partial** — it lists only the keys to change. A dict is
  merged key-by-key; any scalar / list value **replaces** the default.
- So `{"nodes":{"ptp4l":{"args":"X"}}}` changes `ptp4l.args` only, leaving
  `ptp4l.enabled` (and every other node) at the prior value.
- `_comment*` keys (operator notes) are **stripped** from the pushed result —
  they document the override file but never reach the device.
- A target override that names an FC with no machine-generic base logs a
  WARNING (likely a stale filename, or an FC that doesn't run on this machine).

> **Migration note.** Overrides used to ALSO be merged at install/manifest time
> by `theia install`/`theia manifest`, baked into `dist/manifest/<machine>/`.
> That's gone — it coupled the build-once artifact to a rig's config. If you see
> `WARNING: deploy/config/<machine>/ is NOT baked into the artifact` during a
> build, that's expected: the override is now applied at deploy time. A per-RIG
> change belongs under `deploy/config/<target>/`.

## TSYNC time hierarchy (the worked example)

`tsync` is a `{Provider; Controller}` FC whose prebuilt Providers fork
third-party daemons; each Provider's `do_start` is config-driven (reads
`enabled` + `args` from its node section). The `.art` defaults are the **slave**
profile (`ptp4l -s`, `gpsd` disabled).

- `central/tsync.json` (machine layer) — **GPS-disciplined PTP grandmaster**:
  `gpsd` ON; `ptp4l` drops `-s` (`-i ptpIf0 -m -q`) so it becomes
  master/grandmaster.
- `compute/tsync.json` (machine layer) — **PTP slave**: `gpsd` OFF (explicit);
  `ptp4l -s` syncs FROM central over Ethernet.
- `rpi4/tsync.json` (target layer) — a `central`-slice rig with RTK GPS, no PTP
  NIC: disables `ptp4l`/`phc2sys`/`gpsd` outright (on top of central's args).
