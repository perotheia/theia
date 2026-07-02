#!/usr/bin/env python3
"""ucm-adopt.py — hand a Mender-landed release to the on-device UCM agent.

The theia-release update module already staged /opt/theia/releases/<ver> and
flipped `current` → it (the bits are on disk). This tells UCM to ADOPT that
release: run the AUTOSAR lifecycle Mender's bare symlink switch skips —
supervised restart of the affected FCs → PHM-health verify → COMMIT or ROLLBACK.

UCM's UcmGate.EvStartUpdate reads only the manifest VERSION (the "download" is a
no-op when the release is pre-staged — exactly the Mender case), so we pass
artifact_path = the current release dir (already present) and let UCM's idempotent
stage/switch run over it (release_staged()==true → switch re-aims current to the
same target + records `previous` for rollback). The valuable work UCM adds is the
restart + PHM-verify window after that.

This is the Mender→UCM bridge the ArtifactInstall_Leave state-script invokes. It
speaks the gen_server wire via artheia.probe (the same transport tdb/campaign.sh
use) — NEVER raw TIPC. Returns non-zero if UCM rejects, so the state-script can
fail the Mender install (→ ArtifactRollback restores the previous symlink).

Usage:  ucm-adopt.py <version> [full|partial] [fc]
  THEIA_ROOT (default /opt/theia) locates artheia + tdb.art + the release dir.
"""
import os
import sys
from pathlib import Path

ROOT = Path(os.environ.get("THEIA_ROOT", "/opt/theia"))


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: ucm-adopt.py <version> [full|partial] [fc]", file=sys.stderr)
        return 2
    version = sys.argv[1]
    scope = sys.argv[2] if len(sys.argv) > 2 else "full"
    fc = sys.argv[3] if len(sys.argv) > 3 else "theia"

    # UpdateScope: US_FULL=0, US_PARTIAL=1. UpdateKind: UK_SOFTWARE=0.
    us = 1 if scope == "partial" else 0
    uk = 0  # a Mender artifact is always a software package

    # artheia probe (the transport-agnostic gen_server client). tdb.art's TdbUcm is
    # a DISTINCT source node so we don't impersonate UcmDaemon's own address.
    sys.path.insert(0, str(ROOT / "artheia"))
    try:
        from artheia.gen_server.probe import ArtheiaContext
    except Exception as e:  # noqa: BLE001
        print(f"ucm-adopt: no artheia probe ({e}) — symlink-only install", file=sys.stderr)
        return 0  # standalone / no UCM env: the symlink switch IS the install

    art = ROOT / "system/tools/tdb/tdb.art"
    if not art.is_file():
        print(f"ucm-adopt: {art} missing — symlink-only install", file=sys.stderr)
        return 0

    # The release is already at current → releases/<ver>; pass that as artifact_path
    # (UCM doesn't re-download — the bits are staged).
    artifact_path = str(ROOT / "current")

    ctx = ArtheiaContext(str(art), proto_root=str(ROOT / "platform/proto"))

    # TIPC name-connect load-balances across every port co-bound to UcmDaemon's
    # address; a leftover/stale binding can swallow the first attempts. Retry on a
    # fresh probe (fresh socket) within a budget — the live hand-off needed this.
    last_err = "no attempt"
    for attempt in range(1, 6):
        probe = ctx.probe("TdbUcm").start()
        try:
            rep = probe.call(
                "UcmDaemon", "RequestUpdate", timeout=6.0,
                name=fc, version=version, kind=uk, scope=us,
                artifact_path=artifact_path, signature="", has_migrations=False)
            status = rep.get("status") if isinstance(rep, dict) \
                else getattr(rep, "status", 1)
            if status == 0:
                print(f"ucm-adopt: UCM accepted v{version} ({scope}) — "
                      f"running restart + PHM-verify")
                return 0
            if status == 2:
                print("ucm-adopt: UCM not wired yet (agent starting) — "
                      "symlink stands, will adopt on next campaign", file=sys.stderr)
                return 0  # don't fail the install for a transient not-ready
            print(f"ucm-adopt: UCM REJECTED v{version} (status={status})",
                  file=sys.stderr)
            return 1  # an explicit reject → fail → Mender ArtifactRollback
        except Exception as e:  # noqa: BLE001
            last_err = str(e)
        finally:
            probe.stop()
    # Exhausted retries — UCM not reachable (not deployed here, or all bindings
    # stale). The symlink switch stands; the supervisor adopts `current` on restart.
    print(f"ucm-adopt: UCM unreachable after retries ({last_err}) — "
          f"symlink-only install", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
