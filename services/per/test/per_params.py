#!/usr/bin/env python3
"""Verify the static-params path end-to-end: gen-params JSON -> runtime config
singleton -> node reads it at boot.

Generates per's params JSON, edits push_connect_ms to a distinctive value,
boots per pointed at it (THEIA_CONFIG), and asserts per's boot log reports the
edited value (proving it loaded from JSON, not the .art default). Also checks
the no-config fallback to defaults.

Standalone (no probe needed) — reads per's stdout boot log.
"""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
PER = REPO / "bazel-bin/services/per/main/per"
ARTHEIA = REPO / ".venv/bin/artheia"
ART = REPO / "system/services/per/package.art"


def boot_log(env_extra: dict) -> str:
    # per is a daemon — it never exits, so run it briefly and capture whatever
    # it logged at boot (the params line prints in PerClient::init).
    env = {**os.environ, "THEIA_LOGGER": "stdio", **env_extra}
    p = subprocess.Popen([str(PER)], env=env, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True)
    try:
        out, _ = p.communicate(timeout=2)
    except subprocess.TimeoutExpired:
        p.terminate()
        try:
            out, _ = p.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            p.kill()
            out, _ = p.communicate()
    return out or ""


def main() -> int:
    # 1. Generate + edit the params JSON.
    with tempfile.TemporaryDirectory() as d:
        cfg = Path(d) / "per.json"
        subprocess.run([str(ARTHEIA), "gen-params", str(ART), "--out", str(cfg)],
                       check=True, capture_output=True)
        data = json.loads(cfg.read_text())
        data["nodes"]["per_client"]["push_connect_ms"] = 777
        cfg.write_text(json.dumps(data, indent=2))

        # 2. Boot per pointed at it — expect push_connect_ms=777.
        log = boot_log({"THEIA_CONFIG": str(cfg)})
        has_777 = "push_connect_ms=777" in log
        print("with config -> push_connect_ms=777:", "OK" if has_777 else "FAIL")
        if not has_777:
            print(log)

    # 3. No config -> falls back to the .art default (250).
    log = boot_log({"THEIA_CONFIG": "/nonexistent/per.json"})
    has_default = "push_connect_ms=250" in log
    print("no config   -> push_connect_ms=250 (default):",
          "OK" if has_default else "FAIL")

    return 0 if (has_777 and has_default) else 1


if __name__ == "__main__":
    raise SystemExit(main())
