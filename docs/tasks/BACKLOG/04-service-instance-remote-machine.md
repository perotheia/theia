# Populate `ServiceInstance.remote_machine` for strict per-machine service filter

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
