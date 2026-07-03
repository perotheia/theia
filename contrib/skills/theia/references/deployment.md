# Deployment — the installable framework (ROS2-style package split)

Theia ships as a set of **independent Debian packages** under `/opt/theia`,
the way ROS2 ships under `/opt/ros/<distro>`. The point is **name- and
location-independence**: a user installs Theia on a fresh machine from
`.deb`, sources `setup.sh`, and works in their OWN workspace — authoring
`.art`, running the generators + Bazel build, and deploying — without
vendoring any of Theia into their repo.

This is distinct from the in-repo monorepo build (`pip install -e artheia`
→ `bazel build` → `theia install`) and from the per-host deploy bundles in
[provision-orchestrate.md](provision-orchestrate.md). This page is about the
**framework packages** and the **dev / runtime / services / workspace**
boundary.

## The split: machine vs -dev, by audience

The cut follows a single rule — **a machine that only RUNS Theia gets
binaries and nothing else** (no sources, headers, or protos); everything
build-time lives in a matching `-dev` package that `Depends:` the machine
one. Four audiences:

| Audience | Installs | Gets |
| --- | --- | --- |
| **Run-only target** (deploy machine, docker, Pi) | `theia-runtime` [`+ theia-services`] | `supervisor` + service binaries + bundled libs. **Zero build files.** |
| **App developer** (builds apps on Theia) | + `theia-framework` + `theia-runtime-dev` [`+ theia-services-dev` `+ theia-system-dev`] | the above + the `artheia` **wheel**, runtime sources/headers, protos, `.art` tree, Python manifest, the full `system/` contract tree |
| **Test author** | + `theia-rf` | the `rf_theia` harness (minus `scenarios/_selftest`) |
| **Operator** (GUI / live debug) | + `theia-tools` (Ubuntu 24.04) | supervisor-GUI + rtdb (speak com's gRPC) |

The packages and their `Depends:` DAG (clean, acyclic):

```
theia-framework  (artheia + rf wheels + bazel rules + setup.sh; Architecture: all)
      │
      ├─► theia-runtime         supervisor binary                 ← run
      │        └─► theia-runtime-dev    runtime src/hdrs + proto + .art   ← build
      │
      ├─► theia-services        com/per/sm/ucm/log/shwa + libetcd  ← run
      │        └─► theia-services-dev   service protos + .art tree + py manifest ← build
      │                └─► theia-system-dev  full system/ .art tree + .proto (probe clients: tdb/rtdb/rf) ← build
      │
      ├─► theia-tools           GUI + rtdb (need com gRPC; 24.04)  ← operate
      └─► theia-rf              rf_theia harness (Architecture: all) ← test
```

### What each package carries (authoritative: `packaging/theia/BUILD.bazel`)

- **theia-runtime** — `/opt/theia/bin/supervisor` ONLY. `section misc`, no
  build deps. (tdb, a Python probe, is added by `theia release`, not a bazel
  artifact.)
- **theia-runtime-dev** — runtime sources a workspace compiles against:
  `GenServer`/`GenStateM`/`GenRunnable` headers + `.cc`, tombstone, the
  runtime control proto (`.proto` + the **built `.pb.{c,h}`** — git-untracked,
  shipped so the workspace links them without re-running nanopb), and the
  runtime `.art` (defines `ChildControlIf` etc.). Lands at `/opt/theia/src`.
  `Depends: theia-runtime, theia-framework, libnanopb-dev, build-essential`.
- **theia-services** — `/opt/theia/bin/{com,per,sm,ucm,log,shwa}` +
  `/opt/theia/lib/libetcd-cpp-api.so` (per's etcd client, not in apt; the
  postinst `ldconfig`'s it). `Depends: theia-runtime, libgrpc++1,
  libprotobuf23, libcpprest2.10`.
- **theia-services-dev** — service protos (`/opt/theia/proto/...`), the
  services `.art` tree at its FQN-mirrored path **`/opt/theia/system/services`**
  (the workspace's `system/services` symlink target — the SOLE services `.art`
  location; theia-system-dev no longer duplicates it), and the Python services
  manifest as **plain data** at `/opt/theia/manifest/services/{manifest,
  executor}.py` (loaded BY PATH by the generated rig — NOT installed into
  site-packages, NOT on PYTHONPATH). Folds in the former
  `theia-services-manifest`. `Depends: theia-services, theia-framework`.
- **theia-framework** — **wheels-as-data, NO postinst, no system Python.**
  Ships the `artheia` + `rf-theia` wheels (+ all their deps as wheels) under
  `/opt/theia/wheels`, the bazel `rules/`, `toolchains/`, `MODULE.bazel`, the
  `theia`/`artheia*` bin shims, and the POSIX `setup.sh`. The user drops the
  wheels into THEIR OWN venv (`pip install --find-links /opt/theia/wheels
  artheia rf-theia`) — Theia never writes system site-packages. `Architecture:
  all`. Assembled by `theia release` (`_build_framework_deb` in `theia.py`),
  not a bazel target.
- **theia-system-dev** — the `system/` `.art` contract tree **minus the services
  subtree** (`system.art` + supervisor + `tools/tdb` + platform msgs; the
  `//system:art_sources_no_services` filegroup) + the `.proto` sources, for
  probe-backed clients (`tdb`/`rtdb`/`rf-theia`). Its `system.art` imports
  `system.services.*`; the services `.art` those imports resolve against comes
  from **theia-services-dev** (a hard `Depends`), so it's never duplicated here.
  `Depends: theia-services-dev, theia-framework`.
- **theia-rf** — the `rf_theia` harness wheel, MINUS `scenarios/_selftest`
  (excluded by `testing/pyproject.toml` `find.exclude`). `Architecture: all`.

> **Single services `.art` location**: the services tree lives ONLY at
> `/opt/theia/system/services` (owned by theia-services-dev). Earlier builds
> also shipped a flat `/opt/theia/services` copy from services-dev AND the same
> tree inside system-dev's whole-`system/` unit — three copies that could
> diverge. Now services-dev owns the one FQN-mirrored copy and system-dev's
> `art_sources_no_services` excludes it.

## Building the packages — `theia release`

The **4-step build** (framework → runtime → services → package) is driven by
one verb. `.deb` is the default and primary output (Theia is always deployed
on Debian-derived platforms — no QNX); `--ipk` is an opt-in hatch that ALSO
emits the embedded/opkg `.ipk`.

```sh
PATH="$PWD/.venv/bin:$PATH" theia release            # → dist/debian/*.deb
theia release --ipk                                  # + dist/ipkg/*.ipk (embedded hatch)
theia release --arch host,rpi4                       # cross-arch (amd64 + arm64)
theia release --python-only                          # just framework + rf wheels
```

What it does (`cmd_release` / `_RELEASE_BAZEL_PKGS` in `theia.py`):

1. **framework** — `_build_framework_deb`: `pip wheel`s artheia + rf-theia and
   `pip download`s their deps into `/opt/theia/wheels` (no install), ships
   `rules/` + `toolchains/` + `setup.sh`. `Architecture: all`.
2. **rf** — `pip wheel testing/` (the harness, sans `_selftest`).
3. **runtime + services + system** — `bazel build //packaging/theia:theia-runtime_deb
   theia-runtime-dev_deb theia-services_deb theia-services-dev_deb
   theia-system-dev_deb` per `--platforms` (`_RELEASE_BAZEL_PKGS`). With `--ipk`,
   also builds the `*_ipk` machine targets (the `-dev`/`system-dev` packages have
   **no** `.ipk` — `.ipk` is machine-only, the embedded hatch).
4. **collect** — copies `bazel-bin/packaging/theia/*.deb` into
   `dist/debian/<pkg>/` (+ `*.ipk` → `dist/ipkg/`).

Bazel filegroups in `packaging/theia/BUILD.bazel`: `:debs` (all four C++
packages), `:machine_debs` (run-only: runtime + services), `:ipks`.

> If a prior run sudo-staged files into `dist/`, `sudo rm -rf dist/debian
> dist/ipkg` before a fresh release (root-owned leftovers break the copy).

## Installing on a target

`dpkg --install` handles both `.deb` and `.ipk` (same `ar` format). Order
follows the DAG.

```sh
# Run-only machine (no build files land):
sudo apt install ./theia-runtime_*.deb ./theia-services_*.deb

# App-developer workspace (adds the -dev + framework + system-dev packages):
sudo apt install ./theia-framework_*.deb \
                 ./theia-runtime_*.deb  ./theia-runtime-dev_*.deb \
                 ./theia-services_*.deb ./theia-services-dev_*.deb \
                 ./theia-system-dev_*.deb
```

Via Puppet (`deploy/puppet/modules/theia/manifests/pkg_install.pp`): a pure
deploy target installs the machine set; pass `dev => true` to also pull the
`-dev` packages (sequenced after their machine package). See
[provision-orchestrate.md](provision-orchestrate.md) for the remote-install
flow.

After install, `source /opt/theia/setup.sh` — a single POSIX file (works from
bash/zsh/sh), mirroring `/opt/ros/<distro>/setup.bash`. It exports **only**
`THEIA_ROOT` and prepends `/opt/theia/bin` to PATH — it deliberately does NOT
touch PYTHONPATH. `artheia` isn't installed by the deb (it's a wheel; the user
does `python3 -m venv .venv && pip install --find-links /opt/theia/wheels artheia
rf-theia` into their own venv), and the framework's services manifest ships as
plain data at `/opt/theia/manifest/services/` — the generated rig loads it BY
PATH (`_load_services_manifest` in theia.py), so no generic top-level `manifest`
package ever lands on the user's global import namespace. A consuming workspace
then runs `theia init` against it (see the theia skill's "Consuming workspaces"
section).

## The downstream workspace

`theia init [--with-services]` scaffolds a NEW consuming workspace beside the
framework (the gataway_ws/demo pattern). It builds the user's app C++ against
the framework as a **sibling Bazel module** — no vendored runtime, no alias
shims:

```sh
theia init my_ws                                          # scaffold beside theia
artheia gen-app --kind fc system/apps/component.art \
        --out apps --proto-out proto                      # emits @pero_theia//… labels
bazel build //apps/...                                    # compiles against @pero_theia
theia manifest bootstrap && theia install && theia start  # the ws's own rig drives the tree
```

- `MODULE.bazel` is a stripped Theia module (keeps `rules_cc`/`rules_python`/
  `nanopb`/`rules_pkg` + the `rig_ext` extension; drops the PERO
  gateway/PSP/firmware toolchains), consumes the framework via
  `bazel_dep(pero_theia)` + `local_path_override` (relative ws→theia), and
  declares the user's rig (`@rig_myapp`).
- **No platform shims.** gen-app detects the consuming-workspace layout and
  emits the framework labels already qualified —
  `@pero_theia//platform/runtime:runtime`,
  `@pero_theia//platform/supervisor/tombstone:tombstone` — so they resolve
  straight against the sibling module. (Earlier `theia init` wrote per-label
  `alias()` shims under `platform/`; gen-app emitting `@pero_theia//…` directly
  removed them.)
- The app's OWN proto stays local: gen-app writes `proto/system/apps/*` and the
  `//proto:platform_protos` aggregator the lib links is in-workspace (a
  consumer whose `.art` differs from the framework's gets its own wire types).

## Name-independence: rig discovery + aggregator manifest

So Theia doesn't hardcode `apps`/`demo`:

- **rig discovery** — `theia install` finds the deploy rig via
  `_discover_rig_module` (`$THEIA_RIG_MODULE`, else a `*/manifest/rig.py`
  scan that skips `.venv`/`bazel-*`/`artheia`/`templates`/`vendor`); the rig
  name is `$THEIA_RIG` (default `CentralRig`).
- **aggregator-driven manifest** — `gen-manifest` derives the manifest from
  `system/system.art` (the aggregator), resolving empty cluster stubs by
  walking imports to the defining file; each cluster carries its own
  source-tree `base_dir`. It also emits an `executor.py` sidecar (an
  `app_sup` `one_for_one` SupervisorNode holding every app).
- **prebuilt owners** — `$THEIA_PREBUILT_OWNERS` (e.g. `services`) marks a
  cluster non-bazel-buildable; its start_cmd becomes `/opt/theia/bin/<ident>`
  so the rig stages the prebuilt binary from the installed deb instead of
  building it.
- **`gen-rig-combine`** merges a services `.py` manifest + an apps `.py`
  manifest (each with its executor sidecar) into one final `rig.py` —
  combining `<Cluster>Software` specs + the supervisor trees, app_sup
  children Appended into the services tree.

## Mental model

- **`/opt/theia` is the install prefix** — like `/opt/ros/<distro>`. Source
  `setup.sh` to use it; nothing is location-bound.
- **machine package = binaries; `-dev` package = build files.** A deploy
  target never carries sources/protos.
- **`.deb` is primary, `.ipk` is the embedded hatch** (`--ipk`). Same `ar`
  archive; `dpkg` installs either.
- **The workspace consumes Theia, never vendors it** — gen-app emits
  `@pero_theia//…` labels directly (no alias shims), resolving against the
  sibling framework module via `local_path_override`.
- **artheia changes ⇒ rebuild + reinstall `theia-framework`** on the target;
  a stale framework deb breaks `gen-manifest` (e.g. the `_cluster_members`
  tuple-arity drift).
```

