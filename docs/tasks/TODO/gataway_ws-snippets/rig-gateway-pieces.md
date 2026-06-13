# Gateway rig pieces removed from theia.git — recreate in gataway_ws

These were stripped from `apps/manifest/rig.py` + `system/system.art` +
`services/manifest/executor.py` when theia.git went standalone. gataway_ws
(its own rig + aggregator) re-adds them, against the imported Theia clusters.

## system/system.art (gataway_ws aggregator)
```art
import system.gateway.*       // system/gateway → gateway/system (submodule)

composition GatewayBridge  { }

cluster Platform {            // Append GatewayBridge into the imported Platform
    composition GatewayBridge  gw
}
```

## rig.py — SwComponent + Process + drv_sup Append
```python
from artheia.manifest.utils import app_process_for as _app_process_for

GATEWAY_COMPONENT = SwComponent(
    name="gateway",
    bazel_target="//platform/gateway/main:gateway",
    owner="platform",
    art_node="system.gateway/GatewayBridge",
    bazel_buildable=True,
)
GATEWAY_PROCESS = _app_process_for(
    "platform", "gateway",
    ["flexray_bus", "kcan_bus", "gw_svc", "cmp_gw"],
)
# supervisor tree: drv_sup children=["gateway"]  (Append into the imported tree)
# OpkgArtifact(name="gateway", bazel_target="//platform/gateway/main:gateway",
#             target_dir="/opt/theia/gateway/",
#             systemd_unit="/etc/systemd/system/theia-gateway.service")
# PTM: "gateway": CentralHost.name
```

## packaging — see packaging.BUILD.bazel.txt (the pero-gw-* pkg_opkg block)
