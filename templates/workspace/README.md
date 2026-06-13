# Theia workspace template

A minimal **downstream** Theia workspace — what you scaffold AFTER

```sh
# To BUILD apps you need the -dev packages (sources/headers/protos):
sudo apt install ./theia-framework_*.deb \
                 ./theia-runtime_*.deb ./theia-runtime-dev_*.deb
```

to build an OTP-like app system on the Theia supervisor **without** the ARA
services, GUI tools, or the RF test harness. Copy this directory to a new repo
and start authoring `.art` specs.

Theia is split into runtime/-dev: a machine that only RUNS apps installs the
binary-only `theia-runtime` (+ `theia-services`); a workspace that BUILDS apps
adds the `-dev` packages (sources, headers, protos). What's installed here:

| deb | provides | where |
|---|---|---|
| `theia-framework` | the `artheia` CLI (gen-app/gen-proto/gen-manifest/executor emit) + the rig DSL + bazel `rules/` | `pip` (venv) + `/opt/theia` |
| `theia-runtime` | the `supervisor` binary (run-time fabric) + `tdb` | `/opt/theia/bin` |
| `theia-runtime-dev` | runtime sources/headers your app compiles against, `tombstone`, the runtime proto + `.art` | `/opt/theia/src` |

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
