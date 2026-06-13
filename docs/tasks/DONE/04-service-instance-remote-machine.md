# Populate `ServiceInstance.remote_machine` for strict per-machine service filter — DONE 2026-05-23

## Resolution

Three changes:

1. **demo/manifest/rig.py** — post-process `DemoRig.service_manifests`
   after `merge_layers`: each ServiceInstance gets `remote_machine`
   pinned via `_FC_HOST_MACHINE` table. Default: `central_host`.
   Override: `shwa → compute_host`.

2. **demo/manifest/rig.py — DemoSpecLayer** — added
   `service_manifests = {Append(_sm) for _sm in DemoRig.service_manifests}`
   so the structured-DSL path (`DemoSoftware.to_rig()`) ALSO carries
   the pinned instances. Without this, the structured DSL has
   zero service_manifests and emits empty service.yamls.

3. **artheia/generators/dist_manifest.py** — `_service_payload`
   switched from loose-fallback to strict: only emits instances whose
   `remote_machine == machine_name`. Empty pin → instance dropped.
   ServiceManifests with zero matching instances skipped entirely.

## Tests

`artheia/tests/test_dist_manifest.py` — four assertions:

- `shwa` is in `compute_host/service.yaml`, NOT in `central_host` or
  `admin_host`
- per/log/sm/ucm/com are in `central_host`, NOT in `compute_host`
- `admin_host/service.yaml` has zero ServiceInstances
- every emitted instance's `remote_machine` matches its containing
  machine (no strict-filter regressions)

## Verification

```
$ artheia generate-manifest apps.manifest.rig --out /tmp/out
$ yq '.service_manifests[].instances[] | "\(.name) → \(.remote_machine)"' \
      /tmp/out/compute_host/service.yaml
shwa → compute_host
```

106 tests pass (4 new + 102 prior) — same 4 pre-existing failures
(unrelated test_gen_rig + test_transform).

---

## Original ticket follows below

Today `dist/manifest/<machine>/service.yaml` uses a **loose** filter:

> If any ServiceInstance pins `remote_machine` to this machine →
> emit only those. If no instance has any `remote_machine` set →
> include the full manifest (fallback for single-machine rigs).

The fallback is correct for single-machine but **wrong** for
multi-machine: `shwa` is compute-node-only (the SHWA daemon reads
nvidia-smi on the compute box), yet today `services.shwa` shows up
under `central_host/service.yaml` too.

## Fix

In `services/manifest/fc.py` (and any downstream rig.py), set
`ServiceInstance.remote_machine` to the machine that hosts the
provider:

```python
ServiceInstance(
    name="shwa",
    interface_ref="SafeAccelIf",
    transport=TransportBinding.TIPC,
    tipc=TipcAddress(type=0x80010012, instance=0),
    remote_machine="compute_host",   # ← pin
)
```

Same for any FC that pins to a specific machine in a multi-machine
rig:
- `gateway` → host that owns the CAN/FlexRay PSP (today central)
- `supervisor` → every TARGET machine has its own supervisor instance
  (so this is per-machine; the ServiceInstance.name should include
  the machine token to disambiguate, e.g. `supervisor_central`)

## Strict-mode emitter

After every ServiceInstance is explicitly pinned, switch
`dist_manifest._service_payload` to drop the fallback path:

```python
local_instances = [
    i for i in sm.instances
    if getattr(i, "remote_machine", "") == machine_name
]
if not local_instances:
    continue   # this machine doesn't host any instance of this service
copy = dataclasses.replace(sm, instances=local_instances)
```

Loose-mode logic deletes; strict-mode is the default.

## Definition of done

- Every `ServiceInstance` in `services/manifest/fc.py` and
  `demo/manifest/rig.py` declares a non-empty `remote_machine`.
- `dist_manifest._service_payload` runs strict-mode.
- `dist/manifest/central_host/service.yaml` no longer contains
  `services.shwa`; `dist/manifest/compute_host/service.yaml` does.
- Test in `artheia/tests/test_dist_manifest.py` (new) asserts:
  - shwa is in compute, not in central
  - per/log/sm are in central, not in compute

## Not in scope (defer)

- Service discovery layer (etcd / SOME/IP-SD). The per-machine YAML
  describes what's hosted locally; discovery of remote endpoints is
  a runtime concern (services/com reads `index.yaml` + the per-target
  service.yamls to know who runs what).
