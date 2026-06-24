# Config overrides — two layers

This directory holds config overrides deep-merged onto the machine-generic
defaults emitted by `artheia gen-params`. There are TWO layers, applied in order:

1. **per-machine** `deploy/config/<machine>/<fc>.json` — merged at manifest/
   install time (by `theia install` and `theia manifest`). Keyed by the artheia
   `machine` (`central`, `compute`).
2. **per-target** `deploy/config/<target>/<fc>.json` — merged at *deploy* time
   by `theia orchestrate <target>` (tasks/config-override.yml), ON TOP of the
   machine-generic profile. Keyed by the deploy TARGET name (the rig in
   `deploy/registry/<target>.yml`), NOT the machine — so several physical rigs
   can run the same `central` manifest with different config. Example: `rpi4`
   runs the `central` slice but has no PTP NIC, so `deploy/config/rpi4/tsync.json`
   disables ptp4l/phc2sys/gpsd while the real central gateway keeps them on.

## Layout

```
deploy/config/<machine-or-target>/<fc>.json
```

- `<machine>` — the install machine (`central`, `compute`, …), matching the
  machine `theia install` resolves (arg / `$THEIA_MACHINE` / single target).
- `<target>` — a deploy rig name from `deploy/registry/<target>.yml`.
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

`phc2sys` / `chrony` keep their `.art` defaults on both machines.
