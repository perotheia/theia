# Gateway FC — gen-app modernization — DONE

The gateway FC (`platform/gateway/`, its own git repo) was broadly stale
relative to the current gen-app contract (same shape as the supervisor was
pre-port). Now modernized, building, and supervised.

## What was wrong
- `lib/Log.hh` referenced the old Logger shape (`'::platform' has not been
  declared`); lib used `::demo::runtime::` (runtime moved to `::theia::runtime`).
- `lib/gateway_codecs.hh` referenced old proto types.
- gen-app CRASHED on regen: the gateway prototypes the PSP `Kcan_Bus` /
  `Flexray_Bus` mega-nodes as `extern node` forward-decls (the `extern` is a
  workaround for the 1000+-PDU O(N²) parse — the real node body is cached in
  the bus dir's catalog.json). gen-app's node loop hit the extern STUB
  (tipc=None) and crashed at `node.tipc.type`.

## The fix (4 steps, the supervisor-port playbook)
1. **gen-app extern resolution** (artheia `fc_app.py`): the composition
   prototype's `proto.type` cross-ref ALREADY resolves the extern to the real
   PSP node (scope `_resolve_in_component_only` — a node-only projection, ports
   stripped, so the O(N²) PDU parse stays avoided). Added `_resolved_node_by_type`
   + substitute the extern stub with the resolved real node in the node loop
   (unioned across all compositions, so it works with or without `--composition`).
2. **regen** flat: `artheia gen-app --kind fc --out platform/gateway --ns
   ara::gateway --proto-out platform/proto system/gateway/package.art`. lib +
   main modernized; impl/*_handlers.cc preserved (the 283-line CmpGwService PSP
   bridge logic); new impl/<Node>_state.hh emitted.
3. **impl/BUILD.bazel** (hand-owned): added the `gateway_state` cc_library (the
   write-once <Node>_state.hh headers, leaf so lib←impl is a clean DAG, like
   sm_state) + kept the cmpdecoder dep on gateway_impl.
4. **init/handle_info hooks**: the modernized GenServer lib declares
   `init()` + string `handle_info()`; added no-op bodies to the 3 GenServer
   nodes (GatewayService, Kcan_Bus, Flexray_Bus). CmpGwService is a GenRunnable
   (do_start/loop/stop — unchanged). `//platform/gateway/main:gateway` builds.

## Added to the tree + rig
- **drv_sup** (new): a driver/data-path sub-supervisor under core_sup
  (one_for_one), child `gateway`. The gateway bridges the vehicle buses into
  the fabric, so it's its own driver supervisor, not a host service.
- **rig** (demo/manifest/rig.py): re-added the gateway SwComponent +
  OpkgArtifact to `_PLATFORM_FABRIC_COMPONENTS` / `_PLATFORM_OPKG_ARTIFACTS`;
  added a `GATEWAY_PROCESS` (app_process_for, start_cmd=bin/gateway, nodes =
  the 4 GatewayBridge prototypes) into the legacy + structured + CentralRig
  execution manifests. (`_PROCESS_HOST_OVERRIDES` already pinned gateway→central.)
- **theia.py `_LOCAL_BINARIES`**: gateway → //platform/gateway/main:gateway, so
  `theia install` builds + stages bin/gateway.

## Verified live
A bare supervisor launch forks `./bin/gateway`; `[cmp_gw] start:
iface=enp0s31f6 hercules=169.254.8.3 psp_root=/opt/theia/psp filter=...` shows
the params (config/props) flowing. `tdb ps`:
```
drv_sup → gateway_sup → gateway
  flexray_bus 0x9f000001  kcan_bus 0x9f000000  gw_svc 0xa0010001  cmp_gw 0xa0010002
```
(PspLoader.load fails on the dev host — no /opt/theia/psp staged — but the FC
runs; that's a data-staging concern, not a build/wiring one.)
