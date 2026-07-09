# Vehicle application tests

End-to-end tests of vehicle apps that treat the platform as a stable
base — failures here usually mean the app, not the platform.

The in-repo **demo** app's tests do NOT live here. The demo moved into
its own consuming workspace at `demo/`, so its app tests live there:

- `ci/test/gen_chain.robot` — the .art→.ipk gen pipeline test (runs in ci/run.sh s2 against a fresh scaffold)
- `demo/tests/demo_fsm.robot` — the demo FSM drive test
- `demo/tests/per_migration/` — the config-migration example test

This directory is for OTHER vehicle-app tests — vendor-shipped and
integrator-shipped apps that consume the platform from their own
workspaces. (Three framework selftests still use the demo as a fixture
but test FRAMEWORK behavior; they stay under
`testing/scenarios/_selftest/` and reach into `demo/` for the fixture.)
