# Deploy / OTA test suite

Automated coverage for the Puppet→Ansible + Mender OTA features (the deploy stack
migration). Hermetic — **no live rig required** (the end-to-end was hand-verified on
rig1-central; these guard the artifacts from drift and run in CI).

Run:
```sh
python -m pytest testing/deploy/ -q     # 22 tests
```

| file | covers |
|---|---|
| `test_ansible.py` | provision/orchestrate `--syntax-check`, `ansible-lint --profile production`, task-include completeness, inventory, the Mender→UCM bridge shipping. Needs `ansible-core` + `ansible-lint` (auto-skips if absent). |
| `test_theia_release_module.py` | drives the `theia-release` update module through its Mender states; asserts release-dir + atomic-symlink switch, previous-recording, rollback, NeedsArtifactReboot=No / SupportsRollback=Yes (NOT A/B). |
| `test_fleet.py` | `fleet.py` Mender Management-API client — oss(v4)/hosted(v2) path layout, bearer auth, multipart upload framing, deployment creation, CLI. urllib mocked. |
| `test_ucm_adopt.py` | the Mender→UCM hand-off bridge — graceful degradation (exit 0 = symlink-only when no UCM), the state-script's current-release precondition. |

Deps beyond the base venv: `ansible-core`, `ansible-lint` (for `test_ansible.py`).
The module/fleet/ucm-adopt tests use only the stdlib.
