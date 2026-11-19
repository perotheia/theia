# Mender OTA for Theia — release-dir + symlink (NOT A/B partitions)

> **Note (repo split):** this dir now holds ONLY the **on-device** Mender pieces —
> the `theia-release` update module (`modules/`) + the Mender→UCM handoff
> (`state-scripts/`), which ship in theia's runtime `.deb`. The **server side**
> (Mender GW compose, `build-artifact.sh`, `fleet.py`, enrollment) moved to the
> **`ground-station`** repo; the **provision/orchestrate** playbooks moved to the
> **`colony`** repo. See `docs/tasks/BACKLOG/repo-separation.md`.

Theia does **not** use Mender's rootfs A/B partition flip. We deploy a release as
`/opt/theia/releases/<version>/` plus an atomic `current` symlink switch — the same
model as the on-device UCM agent (`services/ucm/impl/release_dir.hpp`). Mender is
customized to fit this via a **custom update module** (`theia-release`), which is
Mender's supported extension point for non-rootfs payloads.

## Why this works (verified on a real rig with the real mender client)

Mender's update modules are scripts the integrator writes; `mender-artifact write
module-image --type <name>` produces a CUSTOM (non-rootfs) artifact that the module
installs. Verified end-to-end with the **actual `mender` client 3.4.0 on the rpi4
rig** (rig1-central) — full standalone `mender install` / `commit` / `rollback`:

```
install v1.0.0  → current → releases/1.0.0          (com runs v1)
commit          → kept
install v2.0.0  → current → releases/2.0.0           (atomic switch; previous → 1.0.0)
rollback        → current → releases/1.0.0           (com runs v1 again, seconds)
```

`mender-artifact read` confirms `Type: theia-release` (a module-image, NOT
rootfs-image — no A/B). Quirks found on the live run:
- Mender namespaces a module-image's provides as `rootfs-image.theia-release.version`
  — just a provides string, not an actual rootfs.
- `mender install` gates on `/var/lib/mender/device_type` (must match the artifact's
  `theia-rig` compat) + reads `/etc/mender/artifact_info`. On a server-enrolled rig
  these come from enrolment; the Ansible `install-mender.yml` task seeds them so a
  provisioned rig does standalone OTA out of the box.
- During `Download`, Mender presents the payload only as named pipes in
  `<tree>/streams/`; the module is a no-op there and reads `<tree>/files/` in
  `ArtifactInstall` (which Mender materialises by then).

## Files

- **`modules/theia-release`** — the update module. Install to
  `/usr/share/mender/modules/v3/theia-release` (0755) on the rig. Implements:
  - `Download` → **no-op**. During Download Mender exposes the payload only as
    named PIPEs in `<tree>/streams/` (no `files/` yet); since the artifacts are
    small we let Mender auto-save them to `<tree>/files/` instead of streaming.
  - `ArtifactInstall` → extract the payload from `<tree>/files/` into
    `releases/<ver>/`, save `previous`, atomically re-aim `current → releases/<ver>`
    (temp symlink + `rename(2)`). Mender has materialised `files/` by this state.
  - `ArtifactCommit` → keep it (the PHM-verify gate runs in the state-script / UCM)
  - `ArtifactRollback` → restore `current → previous` (rollback in seconds)
  - `SupportsRollback`=Yes, `NeedsArtifactReboot`=No (theia restarts only the
    affected FCs via UCM; it never reboots for an update)
- **`build-artifact.sh`** — packs a theia release tree (`bin/ lib/ config/ …`) into a
  `theia-release` Mender artifact. Wraps `mender-artifact write module-image
  --type theia-release`. Usage:
  `build-artifact.sh <version> <release-dir> [device-type] [out.mender]`.
- **`state-scripts/ArtifactInstall_Leave_01_ucm-request`** — the Mender → UCM bridge.
  After the module lands the bits, this signals `UcmDaemon.RequestUpdate` (reusing
  `deploy/vucm/campaign.sh`'s probe path) so the on-device UCM agent runs the AUTOSAR
  lifecycle: stop/restart the affected FCs → PHM-health verify → ACTIVE or ROLLBACK.
  Non-zero exit → Mender rolls the install back (module restores `previous`).
  **Fail-closed:** UCM unreachable / not wired, or no `theia-migrate` on the rig,
  FAILS the install. A rig that runs no UCM (lab, bare dev box) opts out with
  `THEIA_OTA_ALLOW_NO_UCM=1` in the Mender client's environment — then the symlink
  switch IS the install. An explicit UCM reject always fails.

## Hardening (the payload is untrusted input to root scripts)

All three modules (`theia-release`, `theia-swp`, `theia-app`) carry the same
validation block:

- **Names.** `version.txt` (→ `releases/<ver>`) and the `app.json` name
  (→ `apps/<app>`) must match `^[A-Za-z0-9][A-Za-z0-9._+-]{0,63}$` — one path
  component, no `/`, never `..`. Anything else is refused before any write.
- **No re-install of the running release.** `theia-release` / `theia-swp` refuse a
  version that resolves to what `current` points at (staging would overwrite or
  delete the live tree). Roll forward to a new version instead.
- **Archives.** The tarball is listed (`tar -tv`) before extraction and refused if
  it has an absolute path, a `..` component, or any symlink / hardlink / device /
  fifo member; extraction uses `--no-same-owner --no-same-permissions`.
  **Build release tarballs from plain files and directories only.**
- **Commit health gate (`theia-swp`).** `ArtifactCommit` restarts the supervisor,
  then waits up to `THEIA_OTA_HEALTH_WAIT_S` (default 30) seconds for a
  `supervisor` process whose executable is `$THEIA_ROOT/bin/supervisor` to hold
  one pid for `THEIA_OTA_HEALTH_STABLE` (default 3) consecutive 1 s probes, with
  `current/config/executor.json` present. No signal → exit 1 → Mender runs
  `ArtifactRollback`. `THEIA_OTA_HEALTH_WAIT_S=0` disables the gate (lab only).
- **Rollback with no `previous`** exits non-zero (`theia-release`, `theia-swp`).

Tests (normal user, host or build container, no root, nothing outside a temp dir):

```sh
sh platform/runtime/ota/test/test_modules.sh        # -v shows module output
python3 -m pytest testing/deploy/test_ucm_adopt.py
```

## Role split (see docs/tasks/TODO/ansible-mender-migration.md)

- **Mender** = how the release *arrives*: download, integrity, the standalone
  install/commit/rollback state machine, and on-disk landing (release dir + symlink).
- **UCM** (`services/ucm`) = what theia *does* with a delivered release: the AUTOSAR
  FSM (validate → stage → switch → supervisor restart → PHM verify → ACTIVE|ROLLBACK).
  UCM never downloads — Mender does. Mender never sequences services — UCM does.
- **VUCM** = the fleet/campaign surface = a Mender server deployment group (OTA) plus
  the Ansible controller (provisioning). `deploy/vucm/campaign.sh` drives one update;
  in production a Mender deployment's state-script does the same `RequestUpdate`.

## Quick test (standalone, no server)

```sh
# on the rig, as root:
cp deploy/mender/modules/theia-release /usr/share/mender/modules/v3/ && chmod 755 $_
deploy/mender/build-artifact.sh 2.0.0 /opt/theia/releases/2.0.0 theia-rig 2.0.0.mender
mender install 2.0.0.mender   # → lands releases/2.0.0 + switches current
mender commit                 # keep   (or `mender rollback` to revert)
```
