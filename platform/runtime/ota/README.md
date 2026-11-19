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
  then, within ONE budget of `THEIA_OTA_HEALTH_WAIT_S` (default 60) seconds:
  1. a `supervisor` process whose executable is `$THEIA_ROOT/bin/supervisor`
     holds one pid for `THEIA_OTA_HEALTH_STABLE` (default 3) consecutive 1 s
     probes, with `current/config/executor.json` present;
  2. every process the SWP declares in `app.json` (written by `theia
     release-swp`, laid into `releases/<ver>/app.json`) runs FROM the flipped
     release (`/proc/<pid>/exe` is exactly `<release>/bin/<exe>`: not a deleted
     file, not the previous release's binary) and keeps one pid for
     `THEIA_OTA_APP_STABLE_S` (default 10) consecutive seconds;
  3. the optional `health` hook (below) exits 0.

  Any failure → a `COMMIT FAILED` line naming the process (`MISSING`,
  `RESTARTING`, `OLD BINARY`, `NOT STABLE`, `health hook exit N` / `TIMED OUT`)
  → exit 1 → Mender runs `ArtifactRollback` (current → previous + supervisor
  restart: the previous app version runs again). An SWP that declares no
  processes keeps the supervisor-only gate with a WARNING.
  `THEIA_OTA_HEALTH_WAIT_S=0` disables the whole gate (lab only).

  `app.json` (flat; the module parses it with sed):

  ```json
  {"name": "wc", "artifact": "wc-0.1.0-<abi>", "abi": "<abi>",
   "requires_runtime": "0.3.1", "health": "health",
   "gate": {"wait_s": 180, "stable_s": 20, "hook_timeout_s": 15},
   "processes": [{"name": "ingest", "exe": "ingest"}, {"name": "link", "exe": "link"}]}
  ```

  **Timings:** env (`THEIA_OTA_HEALTH_WAIT_S`, `THEIA_OTA_APP_STABLE_S`,
  `THEIA_OTA_HOOK_TIMEOUT_S`; lab, not capped) > the SWP's `gate` (`release-swp
  --gate`; clamped on the rig to wait_s ≤ 300, stable_s ≤ 60, hook_timeout_s ≤ 60,
  with a log line) > defaults 60 / 10 / 10.

  **Health hook contract** (`release-swp --health P`, or `apps/<app>/health`;
  shipped as `releases/<ver>/health`): run after the processes are stable, as user
  `theia` (the commit fails if the rig has no such user), cwd = the release dir,
  stdin `/dev/null`, env `THEIA_ROOT`, `THEIA_RELEASE`, `THEIA_ARTIFACT` and
  `LD_LIBRARY_PATH=<release>/lib` (the SWP's own libs), killed after
  `hook_timeout_s` (default 10). Exit 0 = healthy; anything else or the timeout
  fails the commit. The LAST stdout line goes into the gate log (`health hook
  said: …`); stderr goes to the deployment log.

  Regression: `sh platform/runtime/ota/test/test_modules.sh`, cases `G1.*`.
- **Rollback with no `previous`** exits non-zero (`theia-release`, `theia-swp`).

## No key material in packages (hardening H)

No software package theia builds carries a private key or the dev CA. One
scanner, `tools/no_keys_gate.py`, gates every packaging path and FAILS the build
on a `*.key` / `*.p12` / `*.pfx`, a `*.pem` with `PRIVATE KEY`, any PEM private
key (RSA/EC/DSA/ENCRYPTED/OPENSSH/PGP) in any file, a certificate with
`O=Theia Dev`, a `tskey-` Tailscale key, an AWS key id or an S3/MinIO secret
assignment, in a directory or any archive (tar/tgz/zip/whl/.deb/.ipk/.mender,
nested, zstd included). The message names the file inside the artifact.

| path | gated |
|---|---|
| `theia release` | runtime debs + manifest upload copy + `manifest.tar.gz`, before any upload |
| `theia release-swp` | payload tarball, `.mender`, `--deb`; failing artifacts are removed |
| `theia release-role` | release tarball + `.mender` |
| `theia dist` | every staged `.deb` (app per-machine and runtime) |
| `theia build-debs` | every `.deb` / `.ipk` / wheel written |
| `theia models push` | the `metadata/` files |
| bazel `pkg_deb` (`rules/deb.bzl`) | the staged tree, inside the build action |
| CI `release-deb.yml` | every `.deb` before upload |

`--allow-dev-certs` tolerates the dev cert set (dev-CA certs + keys inside a
`certs/` dir) for a LOCAL dev bundle; every verb refuses it with `--s3`.
`release-swp` no longer copies `dist/manifest/<machine>/certs` into the SWP unless
`--allow-dev-certs` is given.

On the rig: the `theia-swp` module no longer lays `config/certs` (it warns if a
payload still carries `certs/`), and `theia-run.sh` reads certificates ONLY from
the fleet PKI (`/etc/theia/pki/{device.crt,device.key,ca.crt}`); the legacy
`current/config/certs` branch is gone. A rig without a fleet certificate now
refuses to start com (`THEIA_COM_TLS_REQUIRED=1`) instead of falling back to the
dev certs: run the provision / `pki-enroll` first (debug only: `THEIA_COM_INSECURE=1`).

**Migration for rigs that have the old certs.** Rigs installed from an SWP built
before this change hold the dev cert set, PRIVATE keys included, in
`/opt/theia/releases/<ver>/config/certs/` (and `current/config/certs` through the
symlink); `theia-run.sh` logs `ignoring legacy …/config/certs` while one exists.
1. Enroll the rig in the fleet PKI (colony `tasks/pki-enroll.yml`) BEFORE the
   next restart, so com finds `/etc/theia/pki`.
2. Remove the dev material: `rm -rf /opt/theia/releases/*/config/certs`.
3. Treat the dev CA as compromised: it was published (2026-11-19, runtime bucket);
   nothing may trust `O=Theia Dev` outside a developer's own box.

Follow-up (colony, NOT done here): add a cleanup task to the orchestrate play that
removes `<manifest>/certs/*.key` (and `/opt/theia/releases/*/config/certs`) from
every rig and from the controller's S3 manifest cache.

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
