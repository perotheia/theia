#!/usr/bin/env bash
# make-null-base-swp.sh — build a NULL-SOFTWARE base SWP (the day-2 resting
# release for the two-plane OTA model, docs/design/two-plane-ota.md).
#
# A null-software SWP carries the services-baseline executor with NO FC nodes
# (an empty/absent applications_sup) and NO binaries — the module assembles the
# release's bin from the current runtime release. `current → releases/base-<ver>`
# is the resting SWP state: day-2 (install base) and day-3 (install an app SWP)
# are then symmetric current-flips.
#
# Usage: make-null-base-swp.sh <runtime-executor.json> <ver> <requires-runtime> [outdir]
#   <runtime-executor.json>  the runtime release's services baseline executor
#   <ver>                    e.g. base-0.1.0-noble-amd64
#   <requires-runtime>       e.g. 0.3.2-noble-amd64
set -euo pipefail
SRC="${1:?runtime executor.json}"; VER="${2:?version}"; RTREQ="${3:?requires-runtime}"
OUT="${4:-/tmp/$VER}"
rm -rf "$OUT"; mkdir -p "$OUT/manifest/master"

python3 - "$SRC" "$OUT/manifest/master/executor.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
# NULL SOFTWARE: drop applications_sup entirely (no FC nodes), keep services_sup.
def strip_apps(n):
    if isinstance(n, dict):
        for k in ("children", "workers", "nodes"):
            kids = n.get(k)
            if kids:
                n[k] = [c for c in kids
                        if not (isinstance(c, dict) and c.get("name") == "applications_sup")]
                for c in n[k]:
                    strip_apps(c)
strip_apps(d)
json.dump(d, open(sys.argv[2], "w"), indent=2)
print("null-software executor: services baseline, no applications_sup")
PY
echo '{"app":"base","roles":["master"],"arity":1,"on":["master"],"machines":["master"]}' > "$OUT/manifest/machines.json"
echo "$VER"   > "$OUT/version.txt"
echo "$RTREQ" > "$OUT/requires_runtime.txt"

( cd "$OUT" && tar czf "/tmp/$VER.tar.gz" manifest version.txt requires_runtime.txt )
MA="$(command -v mender-artifact-wrap || command -v mender-artifact)"
"$MA" write module-image --type theia-swp --artifact-name "$VER" --device-type theia-rig \
  --file "/tmp/$VER.tar.gz" --file "$OUT/version.txt" --file "$OUT/requires_runtime.txt" \
  --output-path "/tmp/$VER.mender"
echo "wrote /tmp/$VER.mender (null-software base SWP)"
