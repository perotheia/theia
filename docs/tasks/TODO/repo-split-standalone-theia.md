# Depart google-repo: standalone theia.git + per-project submodule workspaces

Two stories, one structural move: **stop using `google-repo` to glue
independent repos into one tree.** `theia.git` becomes a standalone repo
(ROS2-style); downstream *workspaces* consume it as a sibling source tree (for
now — installed `/opt/theia` deb later, once PSP+gateway stabilize).

## Target layout

```
~/repo/launch-box/
  theia/             pero_theia.git  (standalone)
       artheia/                    → submodule  PG50/artheia
       third_party/etcd-cpp-apiv3/ → submodule  github etcd-cpp-apiv3 (tag v0.2.14)
  gataway_ws/        gataway_ws.git  (new — the gateway + vendor project)
       platform/gateway/                  → submodule  ccstheia/pero_cmp_gw_svc      @ main
       gateway/firmware/pero_cmp_ti/      → submodule  ccstheia/pero_cmp_ti          @ main
       gateway/firmware/pero_cmp_ti_gw/   → submodule  ccstheia/pero_cmp_ti_gw       @ main
       gateway/demo/pero_cmp_gw_cln_demo/ → submodule  ccstheia/pero_cmp_gw_cln_demo @ main
       gateway/libs/pero_cmp_lnx/         → submodule  ccstheia/pero_cmp_lnx         @ main
       vendor/autosar/mlbevo_gen2/        → submodule  ccstheia/mlbevo_gen2_cmp_psp  @ mlbevo_gen2-v8.17.02f
       (consumes ../theia as sibling source)
  odd-path-monitor/  PG50/odd-path-monitor.git  (existing, already a foreign-theia-lib consumer)
       (consumes ../theia as sibling source)
```

Both stories progress together and are checkable: `theia/` must build + test
standalone; `gataway_ws/` + `odd-path-monitor/` must build against the sibling
`theia/` source.

## Why this is clean (verified against the build graph)

- `bazel query "somepath(//packaging/theia:debs, //platform/gateway/...)"` →
  **EMPTY**. The Theia framework (runtime, supervisor, services, the
  `packaging/theia` debs) has **zero** build-graph dependency on any
  gateway/autosar/vendor repo. The `libgw` sever + `TheiaMsgHeader.hh` +
  the `theia-framework`/`-runtime`/`-services` deb split already did the work.
- `autosar` and `odd_path_monitor` are ALREADY optional — pulled by
  `.repo/local_manifests/*.xml`, not the default manifest. The gateway is the
  only periphery member still woven into the CORE tree.

## The couplings to cut (theia.git side) — all in the periphery

| # | Coupling | File | Action |
|---|---|---|---|
| 1 | `import system.gateway.*`; `composition GatewayBridge {}`; `GatewayBridge gw` in `cluster Platform` | `system/system.art:34,44,69` | Remove. `cluster Platform` keeps only Supervisor + ComputeSupervisor. A consuming workspace's own aggregator adds GatewayBridge back. |
| 2 | `gateway` SwComponent + `GATEWAY_PROCESS` in the demo rig | `apps/manifest/rig.py:189-205` | Remove from theia's `apps/` rig (apps is Theia's own CI demo, no gateway). gataway_ws's rig declares it. |
| 3 | `drv_sup children=["gateway"]` | `services/manifest/executor.py:74` | Drop to `children=[]` (drv_sup stays as the driver mount-point; gataway_ws Appends `gateway`). |
| 4 | pero-gw `.ipk` package targets ref `//gateway/...`, `//autosar/...` | `packaging/BUILD.bazel` | Delete the pero-gw block (the `theia-*` framework debs in `packaging/theia/BUILD.bazel` are untouched). Move to gataway_ws. |
| 5 | `system/` symlinks `gateway`, `autosar`, `odd_path_monitor` | `system/gateway`, `system/autosar`, `system/odd_path_monitor` | Remove. They dangle once the source trees leave; gataway_ws/odd-path-monitor recreate the ones they need. |
| 6 | google-repo manifest entries for the 6 repos | `.repo/manifests/default.xml`, `.repo/local_manifests/*.xml` | Retire (theia.git no longer provisions them). Keep as historical reference until gataway_ws is proven. |
| 7 | MODULE.bazel header comment lists `//pero_cmp_*`, `//mlbevo_*` | `MODULE.bazel:5-11` | Trim the comment to the standalone set. |

After 1–5, `theia/` parses + builds + tests with NO gateway present. `drv_sup`
is an empty driver supervisor (correct — it's a mount-point a downstream
workspace fills).

## theia.git → standalone repo (submodules)

`theia.git` already has its own remote (`PG50/pero_theia`). Convert the two
in-tree sibling checkouts to submodules:

```sh
cd ~/repo/launch-box/theia          # = pero_theia working copy
git submodule add ssh://git@cicd.skyway.porsche.com/PG50/artheia artheia
git submodule add https://github.com/etcd-cpp-apiv3/etcd-cpp-apiv3 third_party/etcd-cpp-apiv3
cd third_party/etcd-cpp-apiv3 && git checkout refs/tags/v0.2.14 && cd -
git add .gitmodules && git commit -m "repo: standalone theia.git with artheia + etcd submodules"
```

> artheia is editable-installed (`pip install -e artheia/`) — a submodule
> working copy supports that unchanged. Commit artheia on its own remote, then
> bump the submodule pointer in theia.git (the existing two-repo discipline,
> now formalized as a submodule SHA).

## gataway_ws.git (you create the GitLab repo, then)

```sh
cd ~/repo/launch-box && git init gataway_ws && cd gataway_ws
git submodule add .../pero_cmp_gw_svc       platform/gateway
git submodule add .../pero_cmp_ti           gateway/firmware/pero_cmp_ti
git submodule add .../pero_cmp_ti_gw        gateway/firmware/pero_cmp_ti_gw
git submodule add .../pero_cmp_gw_cln_demo  gateway/demo/pero_cmp_gw_cln_demo
git submodule add .../pero_cmp_lnx          gateway/libs/pero_cmp_lnx
git submodule add .../mlbevo_gen2_cmp_psp   vendor/autosar/mlbevo_gen2
cd vendor/autosar/mlbevo_gen2 && git checkout mlbevo_gen2-v8.17.02f && cd -
```

gataway_ws owns (NOT in theia.git): its own `system/` aggregator that
`import`s Theia's clusters AND adds `GatewayBridge`; a rig.py declaring the
gateway SwComponent + GATEWAY_PROCESS + the `drv_sup` Append of `gateway`; the
pero-gw `packaging/BUILD.bazel` block; the `system/{gateway,autosar}` symlinks.

### Consuming Theia as sibling source (the interim, pre-deb path) — `theia init`

PSP + gateway aren't finished, so `/opt/theia` debs aren't viable yet. The
interim path consumes Theia as a SIBLING SOURCE checkout via a catkin-style
init, NOT vendored:

```sh
cd ~/repo/launch-box/gataway_ws
source ../theia/setup.sh        # exports THEIA_ROOT, puts `theia` on PATH
theia init                      # scaffolds system/system.art + manifest/rig.py
                                # + symlinks system/{runtime,services,supervisor}
                                #   into $THEIA_ROOT (clean ../../theia/... links)
```

- `setup.sh` (theia.git root) is the `devel/setup.bash` analogue — sourcing it
  exports `THEIA_ROOT` (the checkout), prepends `.venv/bin` + the `theia`
  launcher to PATH, and puts artheia on PYTHONPATH.
- `theia init` (theia.py verb) scaffolds the CWD: `system/system.art`
  (imports the Theia clusters + a stub you hand-fill), `manifest/rig.py` stub,
  and the runtime/services/supervisor symlinks. It refuses to init the Theia
  checkout itself; it never overwrites an existing file.
- The `odd-path-monitor` `theia_client/` (on taycann) already proves the
  foreign-theia-lib shape; `theia init` formalizes it.

Migrate to the deb path later by setting `THEIA_ROOT=/opt/theia` — the
symlinks + rig stay the same.

### gataway_ws is the runtime + single-app case

gataway_ws = Theia runtime + ONE application (the gateway). The flow:
1. `theia init` (scaffolds system/system.art).
2. Link `gateway/system` → `system/gateway`; `import system.gateway.*`; add
   `GatewayBridge` to `cluster Platform` (hand-edit — see the gateway snippets).
3. `artheia gen-manifest` → the gateway executor.py sidecar (single app).
4. `manifest/rig.py` imports the sidecar as-is.
5. Only THEN put the gateway pieces into rig.py if still needed after the
   generated manifest covers them. (Snippets preserved in
   docs/tasks/TODO/gataway_ws-snippets/ when the work moved out of theia.git.)

## Verification

- **theia.git standalone**: `artheia parse system/system.art` clean (no
  `system.gateway`); `bazel build //packaging/theia:debs`; `tdb`/`rtdb` import;
  `artheia check-addresses system/system.art` OK; rf `_selftest` green.
- **gataway_ws**: `git clone --recursive`; its aggregator parses (Theia clusters
  + GatewayBridge resolve via the sibling override); `bazel build //platform/gateway/main:gateway`.
- **odd-path-monitor**: unchanged — confirm it still builds against `../theia`.

## Open / deferred

- Move to installed-deb consumption once PSP+gateway stabilize (drop the
  sibling `local_path_override`).
- `pero_cmp_lnx` (libs): submodule (actively edited) vs deb (if it stabilizes).
- Whether `vendor/tornado` / other rigs get their own `*_ws` repos by the same
  pattern.
