# Theia workspace template

A minimal **downstream** Theia workspace — what you scaffold AFTER

```sh
sudo apt install ./theia-framework_*.deb ./theia-runtime_*.deb
```

to build an OTP-like app system on the Theia supervisor **without** the ARA
services, GUI tools, or the RF test harness. Copy this directory to a new repo
and start authoring `.art` specs.

What's installed by the two debs:

| deb | provides | where |
|---|---|---|
| `theia-framework` | the `artheia` CLI (gen-app/gen-proto/gen-manifest/executor emit) + the rig DSL + bazel `rules/` | `pip` (venv) + `/opt/theia` |
| `theia-runtime` | runtime sources/headers your app compiles against, the `supervisor` binary, `tombstone`, the runtime proto, and `tdb` | `/opt/theia/src`, `/opt/theia/bin` |

This template wires Bazel so the labels `gen-app` emits —
`//platform/runtime:runtime` and `//platform/supervisor/tombstone:tombstone` —
resolve to the **installed** runtime under `/opt/theia/src` (see
`platform/runtime/BUILD.bazel` + `platform/supervisor/tombstone/BUILD.bazel`).
You do NOT vendor the runtime into your repo.

## Flow

```sh
# 1. scaffold an FC from your .art (generates lib/ impl/ main/ + BUILD files)
artheia gen-app --kind fc system/myapp/package.art --out myapp --ns my::app

# 2. build it against the installed runtime
bazel build //myapp/...

# 3. stage + run under the installed supervisor (your own rig drives the tree)
theia install
theia start
tdb ps
```

## Files

- `MODULE.bazel` — stripped Theia module: keeps `rules_cc`/`rules_python`/`nanopb`/
  `rules_pkg` + the `rig_ext` extension; declares YOUR rig (`@rig_myapp`). No PERO
  gateway/PSP/firmware toolchains.
- `platform/runtime/BUILD.bazel`, `platform/supervisor/tombstone/BUILD.bazel` —
  thin `cc_library` wrappers over the installed deb so the gen-app labels resolve.
- `system/myapp/package.art` — a sample one-node FC spec to copy.
- `manifest/rig.py` — your deploy rig (one machine to start).
