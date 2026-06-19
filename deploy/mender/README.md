# Mender OTA for Theia — release-dir + symlink (NOT A/B partitions)

Theia does **not** use Mender's rootfs A/B partition flip. We deploy a release as
`/opt/theia/releases/<version>/` plus an atomic `current` symlink switch — the same
model as the on-device UCM agent (`services/ucm/impl/release_dir.hpp`). Mender is
customized to fit this via a **custom update module** (`theia-release`), which is
Mender's supported extension point for non-rootfs payloads.

## Why this works (tested on a real Mender install)

Mender's update modules are scripts the integrator writes; `mender-artifact write
module-image --type <name>` produces a CUSTOM (non-rootfs) artifact that the module
installs. We verified end-to-end on a Mender 3.0.0 / mender-artifact 3.6.0 box:

```
INSTALL v1.0.0  → current → releases/1.0.0          (com runs v1)
INSTALL v2.0.0  → current → releases/2.0.0          (atomic switch)
                  previous → releases/1.0.0
ROLLBACK        → current → releases/1.0.0          (com runs v1 again, seconds)
```

`mender-artifact read` confirms `Type: theia-release` (a module-image, NOT
rootfs-image — no A/B). The only quirk: Mender namespaces the artifact's provides
as `rootfs-image.theia-release.version` for any module-image; it's just Mender's
provides string, not an actual rootfs.

## Files

- **`modules/theia-release`** — the update module. Install to
  `/usr/share/mender/modules/v3/theia-release` (0755) on the rig. Implements:
  - `Download` → stage the payload tarball into `releases/<ver>/`
  - `ArtifactInstall` → save `previous`, atomically re-aim `current → releases/<ver>`
    (temp symlink + `rename(2)`)
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
  Standalone (no UCM up) → no-op success; the symlink switch IS the install.

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
