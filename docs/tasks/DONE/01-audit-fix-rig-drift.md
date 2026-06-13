# Fix `artheia audit-manifest` gaps in apps.manifest.rig — DONE 2026-05-23

## Resolution

- `_DEMO_PROCESSES`: art-class names updated `DemoP[1-3]Composition`
  → `Demo3WayP[1-3]` (the new cluster-driven composition names);
  hosted-prototype lists updated `counter_p1` etc. → bare `counter`,
  `driver`, `ticker`, `observer`, `incrementer`.
- Added `_PLATFORM_FABRIC_COMPONENTS`: SwComponents for `supervisor`
  (`//platform/supervisor:ipk`, art_node
  `system.supervisor/Supervisor`) and `gateway`
  (`//platform/gateway:ipk`, art_node
  `system.gateway/GatewayBridge`). Concatenated onto `DEMO_COMPONENTS`
  so they merge in via the legacy DemoLayer.
- New regression test `artheia/tests/test_audit_manifest.py` spawns
  `artheia audit-manifest …` as a subprocess and asserts exit 0 with
  "no gaps". Will catch future .art/rig drift.

## Verification

```
$ artheia audit-manifest platform/system/system.art apps.manifest.rig
art: platform/system/system.art
rig: apps.manifest.rig -> 'demo'

clusters: 3  compositions: 11  prototypes-with-process: 5
rig: applications=2  sw_components=23  processes=21

✓ no gaps — rig is aligned with art
```

Test suite: 102 pass / 4 pre-existing failures (test_gen_rig +
test_transform — unrelated to this task).

---

## Original ticket follows below

The Phase 0 audit (committed in `system .art reshape onto cluster
primitive + Puppet provisioning fields`) surfaced 10 drift gaps
between `platform/system/system.art` and `demo/manifest/rig.py`:

```
artheia audit-manifest platform/system/system.art apps.manifest.rig
```

returns exit 1 with:

```
cluster_member_without_application_or_swcomponent:
  - cluster Demo3Way.p1  (composition Demo3WayP1)
  - cluster Demo3Way.p2  (composition Demo3WayP2)
  - cluster Demo3Way.p3  (composition Demo3WayP3)
  - cluster Platform.sup  (composition Supervisor)
  - cluster Platform.gw  (composition GatewayBridge)

composition_without_swcomponent:
  - composition Demo3WayP1
  - composition Demo3WayP2
  - composition Demo3WayP3
  - composition GatewayBridge
  - composition Supervisor
```

## Two root causes

### 1. SwComponent `art_node` drift

`demo/manifest/rig.py` declares:

```python
SwComponent(name="demo_p1", art_node="system.demo/DemoP1Composition", …)
```

But the .art (after the cluster reshape) now defines the composition
as `Demo3WayP1` (not `DemoP1Composition`). Update each SwComponent's
`art_node`:

```python
art_node="system.demo/Demo3WayP1"   # was DemoP1Composition
art_node="system.demo/Demo3WayP2"   # was DemoP2Composition
art_node="system.demo/Demo3WayP3"   # was DemoP3Composition
```

### 2. No SwComponent for platform-fabric compositions

`cluster Platform` references `composition Supervisor` and
`composition GatewayBridge`, but the rig has no SwComponent that
points at either. Add them — owner=`platform`, bazel_target = the
respective opkg target (matches what the new `Machine.opkg_artifacts`
already declare):

```python
SwComponent(
    name="supervisor",
    bazel_target="//platform/supervisor:ipk",
    owner="platform",
    art_node="system.supervisor/Supervisor",   # composition name
    bazel_buildable=True,
),
SwComponent(
    name="gateway",
    bazel_target="//platform/gateway:ipk",
    owner="platform",
    art_node="system.gateway/GatewayBridge",
    bazel_buildable=True,
),
```

## Definition of done

`artheia audit-manifest platform/system/system.art apps.manifest.rig`
exits 0 with `✓ no gaps — rig is aligned with art`.

Add a CI smoke step (in `artheia/tests/test_audit_manifest.py` or
similar) that runs the audit against `apps.manifest.rig` and asserts
exit 0, so future .art drifts get caught early.
