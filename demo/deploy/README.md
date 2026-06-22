# demo/deploy — manifest-driven Ansible deploy

Deploy ANY rig to its hosts from the **serialized manifest**, with one generic
playbook. Models the user's deploy workflow end-to-end; nothing here is rig- or
machine-specific.

## The flow

```
demo/manifest/<rig>/rig.py          1. the rig — LOGICAL: machines, processes,
   │                                    the supervision tree (committed)
   │  artheia serialize-manifest manifest.<rig>.rig --attr RIG --out demo/build/<rig>
   ▼
demo/build/<rig>/                   2. serialized manifest (generated, gitignored):
   ├── machines.json                   the machine-name list
   └── <machine>/                      per machine:
        ├── machine.json                 arch, cores, machine_states, os_packages
        ├── execution.json               process → {executable label, start_cmd}
        ├── executor.json                 the supervision tree the supervisor runs
        └── config/<fc>.json              static per-FC params
   │
   │  demo/deploy/gen_ansible.py <rig>   ⊕  demo/inventory/<rig>/hosts.ini
   ▼                                        (machine → ssh host/user, committed)
demo/inventory/<rig>/group_vars/<machine>.yml   3. deploy facts (generated):
   │                                    arch, bazel_platform, build_dir, binaries[]
   │  ansible-playbook -i demo/inventory/<rig>/hosts.ini demo/deploy/orchestrate.yml
   ▼
each box                            4. cross-build the machine's binaries for its
                                       arch → stage → executor.json + config →
                                       (re)launch the supervisor
```

## Why the split

- The **manifest** (`demo/build/<rig>/`) carries everything LOGICAL — which
  processes exist, their binaries, the tree, the config — derived from the
  `.art`. It's regenerated, not committed.
- The **inventory** (`demo/inventory/<rig>/hosts.ini`) carries the one thing the
  `.art` can't know: which physical box each machine deploys to (ssh host/user).
  Committed, hand-maintained per rig.
- `gen_ansible.py` JOINS them: each inventory GROUP is named after a manifest
  machine, so the generated `group_vars/<machine>.yml` auto-loads for every host
  in that group. The playbook then reads `arch`/`binaries`/`build_dir` and does
  the same thing for every machine — no per-rig logic.

## Run it (rpi4 example)

```bash
# 1. serialize the rig to demo/build/rpi4/
artheia serialize-manifest manifest.rpi4.rig --attr RIG --out demo/build/rpi4
#    (or: theia manifest rpi4 --out demo/build/rpi4)

# 2. generate the per-machine Ansible deploy vars from the manifest + inventory
demo/deploy/gen_ansible.py rpi4

# 3. deploy — cross-builds each machine's binaries, stages, launches
ansible-playbook -i demo/inventory/rpi4/hosts.ini demo/deploy/orchestrate.yml
```

`provision` (Phase 1: OS packages + etcd, from `machine.json`) is the existing
`deploy/ansible/provision.yml`, which is already manifest-generic. This dir is
the Phase-2 (binaries + tree + config) orchestrate, rebuilt to be rig-generic.

## Adding a rig

1. Write `demo/manifest/<rig>/rig.py` (the `DeploymentLayer`).
2. Write `demo/inventory/<rig>/hosts.ini` — one `[<machine>]` group per manifest
   machine, with the box's `ansible_host`/`ansible_user`.
3. `artheia serialize-manifest … --out demo/build/<rig>` + `gen_ansible.py <rig>`
   + `ansible-playbook -i demo/inventory/<rig>/hosts.ini orchestrate.yml`.
