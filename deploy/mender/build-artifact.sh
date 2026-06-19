#!/usr/bin/env bash
# deploy/mender/build-artifact.sh — pack a Theia release into a Mender Artifact
# that the `theia-release` update module installs (release-dir + symlink, NOT A/B).
#
# Mender's rootfs-image flips A/B partitions; we use a CUSTOM update module so the
# artifact lands as /opt/theia/releases/<ver> + a current symlink switch (the same
# model as services/ucm). This wraps `mender-artifact write module-image -T
# theia-release` — the supported customization point.
#
# Usage:
#   build-artifact.sh <version> <release-dir> [device-type] [out.mender]
#     <version>      e.g. 2.0.0  (the artifact name; the install dir name)
#     <release-dir>  a dir with bin/ lib/ config/ migrations/ hooks/ (a theia
#                    release tree — e.g. what `theia dist` produces, or an
#                    /opt/theia/releases/<v> snapshot)
#     device-type    default "theia-rig" (Mender compatibility gate)
#     out            default <version>.mender
#
# Produces <out> — `mender install <out>` (standalone) or a Mender server
# deployment then runs the theia-release module → release dir + symlink switch.
set -euo pipefail

VER="${1:?usage: build-artifact.sh <version> <release-dir> [device-type] [out]}"
RELDIR="${2:?need a release dir (bin/ lib/ config/ …)}"
DEVTYPE="${3:-theia-rig}"
OUT="${4:-${VER}.mender}"

[ -d "$RELDIR" ] || { echo "release dir not found: $RELDIR" >&2; exit 1; }
command -v mender-artifact >/dev/null || { echo "mender-artifact not installed" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Pack the release tree + a version marker (the module reads version.txt).
echo "$VER" > "$WORK/version.txt"
# zstd if available (smaller), else gzip.
if command -v zstd >/dev/null; then
    tar -C "$RELDIR" -cf - . | zstd -q -o "$WORK/release.tar.zst"
    PAYLOAD="$WORK/release.tar.zst"
else
    tar -C "$RELDIR" -czf "$WORK/release.tar.gz" .
    PAYLOAD="$WORK/release.tar.gz"
fi

echo "[mender] packing $VER ($RELDIR) → $OUT (device-type=$DEVTYPE, module=theia-release)"
mender-artifact write module-image \
    --type theia-release \
    --artifact-name "$VER" \
    --device-type "$DEVTYPE" \
    --file "$PAYLOAD" \
    --file "$WORK/version.txt" \
    --output-path "$OUT"

echo "[mender] wrote $OUT"
echo "  install standalone:  sudo mender install $OUT && sudo mender commit"
echo "  (the theia-release module lands /opt/theia/releases/$VER + switches current/)"
