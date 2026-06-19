# DEPRECATED — Puppet is replaced by Ansible (provisioning) + Mender (OTA)

The deploy stack moved off Puppet for security: an **agentless** model where a
controller pushes over SSH (`Controller ──SSH──> Device`) with **no standing
convergence agent** on the rig. Mender is now the only persistent on-rig agent,
and it does OTA software updates only.

| Puppet (here) | Replaced by |
|---|---|
| `manifests/provisioning.pp` (Phase 1) | `deploy/ansible/provision.yml` (1:1 translation) |
| `manifests/orchestration.pp` (Phase 2) | `deploy/ansible/orchestrate.yml` (1:1 translation) |
| `manifests/postinstall.pp` (setcap) | `deploy/ansible/tasks/setcap.yml` |
| Puppet-as-VUCM (fleet) | Mender server (OTA) + the Ansible controller (provisioning) |
| `.ipk`/`.deb` push (orchestration) | Mender `theia-release` artifact (release-dir + symlink, NOT A/B) |

Both engines read the **same** `dist/manifest/<machine>/*.json` — artheia stays the
source of truth; only the apply engine changed. `theia provision`/`orchestrate` now
run `ansible-playbook` (a `--puppet` escape hatch keeps this tree usable for one
release). `deploy/run-supervisor.sh` already dropped the in-container puppet driver
("PUPPET IS GONE") — the rig has no agent to converge; re-provision is a
controller-side `ansible-playbook`, field updates come via Mender → the on-device
UCM agent.

See the design + migration plan:
- `docs/tasks/DONE/UCM-VUCM.md` — the three-layer Ansible / Mender / UCM split
- `docs/tasks/DONE/ansible-mender-migration.md` — the phased migration (all phases done)
- `deploy/ansible/` — the playbooks; `deploy/mender/` — the OTA module + server harness

This `deploy/puppet/` tree is kept for one release as the `--puppet` fallback and as
the authoritative step-list the Ansible tasks were translated from. It can be removed
once the Ansible path has shipped a release.
