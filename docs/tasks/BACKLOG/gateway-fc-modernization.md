# Gateway FC — gen-app modernization

The gateway FC (`platform/gateway/`, its own git repo) is broadly stale relative
to the current gen-app contract — same shape as the supervisor was pre-port:

- `lib/gateway_codecs.hh` referenced the old `services_gateway_*` proto types
  (the proto is `system_gateway` now).
- `lib/Log.hh` errors (`'::platform' has not been declared` — old Logger shape).
- A full regen (`artheia gen-app --kind fc --out platform/gateway --ns ara::gateway
  platform/gateway/system/package.art`) brings lib up to date but then the
  HAND-OWNED `impl/BUILD.bazel` is stale: the new lib expects a
  `//platform/gateway/impl:gateway_state` target (the per-node state cc_library,
  like supervisor_state) that the committed impl/BUILD doesn't declare. The
  regen also adds `init()`/`handle_info()` hooks + `impl/<Node>_state.hh` files.

So modernizing gateway = the supervisor-port playbook:
1. regen lib/main from the .art (gen-app).
2. hand-own `impl/BUILD.bazel`: add the `gateway_state` cc_library (the
   write-once `<Node>_state.hh` headers) + wire it into the lib<-impl DAG.
3. seed/keep the `impl/<Node>_handlers.cc` bodies (Kcan_Bus / Flexray_Bus /
   GatewayService / CmpGwService) — these are the real PSP bridge logic.
4. build `//platform/gateway/main:gateway` green.

Until then gateway is DROPPED from the deploy rig (demo/manifest/rig.py +
zonal_rig.py: removed from `_PLATFORM_FABRIC_COMPONENTS` + `_PLATFORM_OPKG_ARTIFACTS`).
Re-add both once it builds. The dist .ipk images + //:install are green without
it (supervisor + FCs + apps). See the deploy-rig-split commit.
