"""RF keyword wrapper over the Mender adapter — the on-device OTA hand-off e2e.

Drives the MenderAdapter (rf_theia.adapters.mender) against a LIVE rig: stage a
release, deliver it (flip current + fire the ArtifactInstall_Leave state-script
→ theia-migrate adopt → UcmDaemon.RequestUpdate), and assert UCM adopted it — the
whole on-device chain the production Mender install runs, minus the Mender
server. Also exercises the fail path (an unreachable UCM → symlink-only, install
still succeeds) and rollback.

Requires: a live rig with UcmDaemon reachable, and $THEIA_ROOT/bin/theia-migrate
present in the install tree (the native adopter that ships in theia-runtime).
Attach to the running rig; never owns the supervisor.
"""
from __future__ import annotations

import os
import sys
from pathlib import Path

from robot.api import logger
from robot.api.deco import keyword, library

# rf-theia ships the adapter; add its package root if not already importable.
_RF = Path(__file__).resolve()
for _p in _RF.parents:
    if (_p / "rf_theia" / "adapters" / "mender.py").is_file():
        sys.path.insert(0, str(_p))
        break
try:
    from rf_theia.adapters.mender import MenderAdapter
except ImportError:  # repo-root fallback
    sys.path.insert(0, str(_RF.parents[4] / "rf-theia"))
    from rf_theia.adapters.mender import MenderAdapter


@library(scope="SUITE")
class MenderAdoptLib:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    def __init__(self) -> None:
        # THEIA_ROOT = the rig's install prefix (…/install/<machine> in a dev ws,
        # /opt/theia on a deb rig). Overridable via MENDER_THEIA_ROOT.
        self._root = os.environ.get(
            "MENDER_THEIA_ROOT",
            os.path.join(os.getcwd(), "install", "central"))
        self._repo = os.environ.get("THEIA_ROOT", "")
        self._m: "MenderAdapter | None" = None

    def _adapter(self) -> "MenderAdapter":
        if self._m is None:
            self._m = MenderAdapter(theia_root=self._root,
                                    repo_root=self._repo or None)
        return self._m

    @keyword("Stage Release")
    def stage_release(self, version: str, bin_src: str = "") -> str:
        """Lay down releases/<version>/ (free-swap: reuse current's bins unless
        bin_src is given) — what the theia-swp module unpacks from a .mender."""
        path = self._adapter().stage_release(version, bin_src or None)
        logger.info(f"staged release {version} at {path}")
        return path

    @keyword("Mender Deliver")
    def mender_deliver(self, version: str) -> int:
        """Flip current → the release + fire the ArtifactInstall_Leave hand-off
        (theia-migrate adopt → UcmDaemon.RequestUpdate), as Mender does. Returns
        the state-script exit code."""
        rc = self._adapter().deliver(version)
        logger.info(f"mender deliver {version} → state-script rc={rc}")
        return rc

    @keyword("Adopt Should Succeed")
    def adopt_should_succeed(self, version: str) -> None:
        """The full deliver → adopt chain must succeed (rc 0) AND leave `current`
        pointing at the delivered version — UCM accepted the RequestUpdate (or,
        on a rig with no UCM, the symlink switch stands). Fails the install
        (non-zero) only on an explicit UCM reject."""
        rc = self._adapter().deliver(version)
        if rc != 0:
            raise AssertionError(
                f"deliver {version} FAILED (state-script rc={rc}) — UCM "
                f"rejected the adopt; Mender would ArtifactRollback")
        cur = self._adapter().current_version()
        if cur != version:
            raise AssertionError(
                f"adopt succeeded but current={cur!r}, expected {version!r}")
        logger.info(f"adopt of {version} succeeded; current={cur}")

    @keyword("Current Release Should Be")
    def current_release_should_be(self, version: str) -> None:
        cur = self._adapter().current_version()
        if cur != version:
            raise AssertionError(
                f"current release is {cur!r}, expected {version!r}")

    @keyword("Mender Rollback")
    def mender_rollback(self) -> str:
        """Restore current → the previous release (Mender ArtifactRollback)."""
        restored = self._adapter().rollback()
        logger.info(f"rolled back; current → {self._adapter().current_version()}")
        return restored
