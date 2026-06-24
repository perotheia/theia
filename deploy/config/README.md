# Per-machine config overrides

This directory holds **per-machine overrides** that `theia install` deep-merges
onto the machine-generic defaults emitted by `artheia gen-params`.

## Layout

```
deploy/config/<machine>/<fc>.json
```

- `<machine>` — the install machine (`central`, `compute`, …), matching the
  machine `theia install` resolves (arg / `$THEIA_MACHINE` / single target).
- `<fc>` — the function-cluster (service) name, e.g. `tsync`.

## How it works

For every FC, `theia install` first runs:

```
artheia gen-params <fc>.art --out install/<machine>/config/<fc>.json
```

which writes the **machine-generic default** — the static per-node `params`
from the `.art`, structured by node section:

```json
{"package": "...",
 "nodes": {"gpsd":  {"enabled": false, "args": "-N -n /dev/ttyGPS0"},
           "ptp4l": {"enabled": true,  "args": "-i ptpIf0 -s -m -q"},
           ...}}
```

If `deploy/config/<machine>/<fc>.json` exists, `theia install` **deep-merges**
it onto that default (`_deep_merge` in `theia.py`) and writes the result back:

- The override is **partial** — it lists only the keys to change. A dict is
  merged key-by-key; any scalar / list value **replaces** the default.
- So `{"nodes":{"ptp4l":{"args":"X"}}}` changes `ptp4l.args` only, leaving
  `ptp4l.enabled` (and every other node) at the `.art` default.
- `theia install` logs
  `applied per-machine config override deploy/config/<machine>/<fc>.json`
  when a merge happens.

## TSYNC time hierarchy (the worked example)

`tsync` is a `{Provider; Controller}` FC whose prebuilt Providers fork
third-party daemons; each Provider's `do_start` is config-driven (reads
`enabled` + `args` from its node section). The `.art` defaults are the **slave**
profile (`ptp4l -s`, `gpsd` disabled).

- `central/tsync.json` — **GPS-disciplined PTP grandmaster**: `gpsd` ON;
  `ptp4l` drops `-s` (`-i ptpIf0 -m -q`) so it becomes master/grandmaster.
- `compute/tsync.json` — **PTP slave**: `gpsd` OFF (explicit); `ptp4l -s`
  (`-i ptpIf0 -s -m -q`) syncs FROM central over Ethernet.
- `gpsfeed/tsync.json` — **GPS-feed-only** (the `manifest.odd_path.rig` box
  feeding odd-path-monitor): `ptp4l` / `phc2sys` / `gpsd` ALL OFF. This box has
  no PTP-capable NIC (no `ptpIf0`), so leaving the daemons on forks children
  that fail + churn on the supervisor's restart strategy. Only tsync's
  in-process GPS broadcaster (`tsync_ctl`) runs and casts NavSatFix/Odometry
  over PG. (Machine is `gpsfeed`, not `central`, precisely so it gets this
  profile instead of central's grandmaster one.)

`phc2sys` / `chrony` keep their `.art` defaults on the central/compute machines.
