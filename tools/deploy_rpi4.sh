#!/usr/bin/env bash
# tools/deploy_rpi4.sh — build + deploy a Theia machine bundle to a Pi 4.
#
# Wraps the canonical workflow: `bazel build` the machine's image
# target, scp the resulting .ipk to the Pi, then `dpkg -i` it. Also
# runs a TIPC modprobe preflight (Raspberry Pi OS 12 ships TIPC as a
# kernel module — modprobe at first deploy, the kernel autoloads on
# AF_TIPC socket() thereafter).
#
# Usage:
#     tools/deploy_rpi4.sh <machine> pi@<host>
#
# Example:
#     tools/deploy_rpi4.sh compute_host pi@192.168.1.42
#
# Assumptions:
#   - The rig is //demo:demo (i.e. @rig_demo). For other rigs, edit
#     RIG_TARGET below or pass --rig.
#   - The Pi runs Raspberry Pi OS 12 (bookworm), aarch64 — matches
#     the sysroot at third_party/sysroot/rpi4/.
#   - `ssh pi@<host>` works without an interactive password prompt
#     (key-based auth or already-cached agent).

set -euo pipefail

RIG_TARGET="@rig_demo"
PLATFORM_FLAG="--platforms=//config:rpi4"

usage() {
    cat <<EOF
Usage: $(basename "$0") [--rig <bazel-target>] [--no-platform] <machine> <ssh-target>

  <machine>     One of the machine names from demo/manifest/rig.py
                (e.g. compute_host).
  <ssh-target>  user@host or just host (e.g. pi@192.168.1.42).

Options:
  --rig <T>      Override @rig_demo with another @rig_<name> target.
  --no-platform  Skip --platforms=//config:rpi4 (use rig-declared arch
                 only — useful when the rig's cc_binary components don't
                 cross-compile yet and you only want the .ipk metadata).
  -h, --help     This message.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --rig)         RIG_TARGET="$2"; shift 2 ;;
        --no-platform) PLATFORM_FLAG=""; shift ;;
        -h|--help)     usage; exit 0 ;;
        --)            shift; break ;;
        -*)            echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
        *)             break ;;
    esac
done

if [[ $# -ne 2 ]]; then
    usage >&2
    exit 2
fi

MACHINE="$1"
SSH_TARGET="$2"

# Resolve workspace + bring up the venv so `artheia rig-deps` (which
# Bazel's repo rule runs) finds itself on PATH.
WORKSPACE="$(cd "$(dirname "$0")/.." && pwd)"
export PATH="$WORKSPACE/.venv/bin:$PATH"

cd "$WORKSPACE"

# ---------------------------------------------------------------- preflight

echo "==> TIPC preflight on ${SSH_TARGET}"
# Two-line check: `modprobe tipc` loads the module (no-op if already
# loaded); `lsmod | grep tipc` confirms presence. Failure here means
# the Pi's kernel doesn't ship TIPC — stock Pi OS 12 does, so this
# is mostly catching custom-kernel boxes. Don't deploy without it.
if ! ssh -o BatchMode=yes "${SSH_TARGET}" \
        'sudo modprobe tipc && lsmod | grep -q "^tipc " && echo "tipc OK"' \
        2>&1; then
    echo "ERROR: TIPC not available on ${SSH_TARGET}." >&2
    echo "       The supervisor needs AF_TIPC sockets. Either:" >&2
    echo "         - apt install linux-modules-\$(uname -r) (Pi OS 12)" >&2
    echo "         - or use a kernel built with CONFIG_TIPC=m" >&2
    exit 3
fi

# ---------------------------------------------------------------- build

echo "==> bazel build ${RIG_TARGET}//${MACHINE}:image ${PLATFORM_FLAG}"
bazel build "${RIG_TARGET}//${MACHINE}:image" ${PLATFORM_FLAG}

# rules_pkg's pkg_opkg writes the .ipk into the per-machine subdir of
# the synthetic @rig_<name>// repo. Resolve it via `bazel cquery`'s
# `--output=files` so we don't have to hardcode the output path.
IPK_PATH="$(bazel cquery "${RIG_TARGET}//${MACHINE}:image" \
                ${PLATFORM_FLAG} --output=files 2>/dev/null | head -1)"

if [[ -z "${IPK_PATH}" || ! -f "${IPK_PATH}" ]]; then
    echo "ERROR: could not locate built .ipk for ${RIG_TARGET}//${MACHINE}" >&2
    echo "       cquery output: ${IPK_PATH}" >&2
    exit 4
fi

IPK_NAME="$(basename "${IPK_PATH}")"
echo "==> built ${IPK_NAME} ($(stat -c%s "${IPK_PATH}") bytes)"

# Sanity-check the .ipk's metadata arch matches what we're deploying to.
# The Pi 4 is aarch64 → expect _arm64.ipk in the name.
if [[ "${IPK_NAME}" != *"_arm64.ipk" ]]; then
    echo "WARNING: ${IPK_NAME} is not arm64 — did you mean to pass" >&2
    echo "         --platforms=//config:rpi4 (or set the machine's" >&2
    echo "         arch=aarch64 in rig.py)?" >&2
fi

# ---------------------------------------------------------------- deploy

REMOTE_PATH="/tmp/${IPK_NAME}"

echo "==> scp ${IPK_NAME} → ${SSH_TARGET}:${REMOTE_PATH}"
scp "${IPK_PATH}" "${SSH_TARGET}:${REMOTE_PATH}"

echo "==> dpkg -i ${REMOTE_PATH}"
ssh "${SSH_TARGET}" "sudo dpkg -i ${REMOTE_PATH}"

echo "==> done. Pi has ${IPK_NAME} installed."
echo "    Next: ssh ${SSH_TARGET} 'sudo systemctl status theia-supervisor' (once the"
echo "          systemd unit lands) — or run the binaries by hand from /usr/bin/."
