# deploy/config/master/ — safe-base override for the `master` role

Deep-merged (by child name) onto the serialized `executor.json` by
`_apply_config_overrides` — the same routine `theia install` and colony's
`config-override.yml` use. Applied at manifest serialization (`theia manifest
services`), so every consumer inherits it: the services deb, the S3 manifest,
and a user SWP that inherits `manifest/services/rig.py`.

## `executor.json` — HW-gated FCs, `run_on_start: false`

These FCs drive host subsystems / caps a bare rig may not have. Booting them by
default crash-loops and, under the root `one_for_all` strategy, escalates the
whole supervisor tree. Emitting them **defined-but-not-booted** makes the
runtime deploy cleanly on ANY rig regardless of HW/CAPA (docker rigs, minimal
boards):

- `fw` — nftables/netfilter (needs the host netns rules + `CAP_NET_ADMIN`)
- `tsync` — ptp4l/phc2sys (needs a PTP clock device + `CAP_SYS_TIME`)
- `rds` — iox-roudi (needs the RouDi shared-memory segment / hugepages)

An operator whose target HAS the subsystem re-enables it with a **per-target**
override (`deploy/config/<target>/executor.json` with `run_on_start: true`),
which merges on top of this base.
