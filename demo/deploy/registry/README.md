# Deploy-target registry

A **target** is a physical rig the controller deploys to. Each target is one
YAML file here, named by the target (`<target>.yml`). `theia orchestrate
<target>` resolves the file to find the host and which manifest slice to push,
then applies the per-target config overrides in `deploy/config/<target>/`.

The target name is INDEPENDENT of the artheia `machine` (the manifest slice).
Several physical rigs can run the same `central` slice with different per-target
config — so the registry maps *rig → {host, machine}* and the config dir keys
overrides off the *rig*, not the machine.

## Format

```yaml
# deploy/registry/<target>.yml
ansible_host: 10.0.0.22        # the rig's IP / hostname (SSH)
ansible_user: axadmin          # SSH user (default: root)
machine: central               # the dist/manifest/<machine>/ slice to push
arch: aarch64                  # informational (the slice already encodes it)
```

## How `theia orchestrate <target>` uses it

1. Reads `deploy/registry/<target>.yml`, `add_host`s the rig in-memory
   (no static inventory line needed).
2. Pushes `dist/manifest/<machine>/` — the `.deb` + executor.json + the
   machine-generic per-FC config.
3. Deep-merges `deploy/config/<target>/<fc>.json` on top (the per-target
   override pass), then reloads the supervisor.

## Adding a target

1. Build the slice: `theia manifest <rig> && theia dist`.
2. Drop a `deploy/registry/<target>.yml` (host + machine).
3. Optionally add `deploy/config/<target>/<fc>.json` overrides.
4. `theia orchestrate <target>`.
