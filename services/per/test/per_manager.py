#!/usr/bin/env python3
"""Drive the PerManager API via artheia.probe.

Registers schemas, lists them, and checks the etcd-backed ops (MigrateBulk /
Snapshot / RestoreSnapshot) return an honest NOT-IMPLEMENTED status (3) rather
than a fake success. Impersonates PerClient to call PerManager's ops.
"""
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "artheia"))

from artheia.gen_server.probe import ArtheiaContext  # noqa: E402

ART = REPO / "system/services/per/package.art"
PROTO = REPO / "platform/proto"


def main() -> int:
    ctx = ArtheiaContext(str(ART), proto_root=str(PROTO))
    probe = ctx.probe("PerClient").start()
    ok = True
    try:
        print("== RegisterSchema(SupervisorConfig, v1) ==")
        r = probe.call("PerManager", "RegisterSchema",
                       config_type="SupervisorConfig", digest="v1")
        print("  ", r); ok &= r.get("status") == 0

        print("== RegisterSchema(SupervisorConfig, v2) ==")
        r = probe.call("PerManager", "RegisterSchema",
                       config_type="SupervisorConfig", digest="v2")
        print("  ", r); ok &= r.get("status") == 0

        print("== RegisterSchema(LogConfig, v1) ==")
        probe.call("PerManager", "RegisterSchema",
                   config_type="LogConfig", digest="v1")

        print("== RegisterSchema(empty) -> should REJECT (status 2) ==")
        r = probe.call("PerManager", "RegisterSchema",
                       config_type="", digest="v1")
        print("  ", r); ok &= r.get("status") == 2

        print("== ListSchemas(SupervisorConfig) -> v1 + v2 ==")
        r = probe.call("PerManager", "ListSchemas", config_type="SupervisorConfig")
        # repeated nested messages decode to protobuf objects -> attribute access
        digs = sorted(s.digest for s in r.get("schemas", []))
        print("  digests:", digs); ok &= digs == ["v1", "v2"]

        print("== ListSchemas(all) -> 3 entries ==")
        r = probe.call("PerManager", "ListSchemas", config_type="")
        n = len(r.get("schemas", []))
        print("  count:", n); ok &= n == 3

        print("== Snapshot -> NOT IMPLEMENTED (status 3) ==")
        r = probe.call("PerManager", "Snapshot", label="pre")
        print("  ", r); ok &= r.get("status") == 3

        print("== MigrateBulk -> NOT IMPLEMENTED (status 3) ==")
        r = probe.call("PerManager", "MigrateBulk",
                       config_type="SupervisorConfig", from_digest="v1", to_digest="v2")
        print("  ", r); ok &= r.get("status") == 3

        print("RESULT:", "OK" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        probe.stop()


if __name__ == "__main__":
    raise SystemExit(main())
