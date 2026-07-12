#!/usr/bin/env bash
# Build tipc.ko out-of-tree for an NVIDIA L4T/tegra kernel (CONFIG_TIPC=n).
# Run ON the board (needs its kernel headers). Proven: jason (5.10.104-tegra,
# AGX) and exo (5.15.148-tegra, Orin Nano).
#
# The tegra kernels ship no TIPC and their net_device struct is compiled
# WITHOUT the tipc_ptr member (it is CONFIG_TIPC-guarded), so upstream
# bearer.c cannot build as-is: patch_bearer.py swaps the 4 tipc_ptr sites for
# the tipc_devmap.h RCU side table (dev-pointer keyed). Minimal object set:
# eth bearer only — no udp_media (udp_tunnel symbols), no crypto, no diag.
set -euo pipefail
KVER=$(uname -r)                    # e.g. 5.15.148-tegra
UPSTREAM=${UPSTREAM:-${KVER%-tegra}}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD=${BUILD:-~/tipc-build}

mkdir -p "$BUILD" && cd "$BUILD"
if [ ! -d net/tipc ]; then
    curl -fsSL -o linux.tar.xz \
        "https://cdn.kernel.org/pub/linux/kernel/v${UPSTREAM%%.*}.x/linux-${UPSTREAM}.tar.xz"
    tar -xf linux.tar.xz "linux-${UPSTREAM}/net/tipc"
    mv "linux-${UPSTREAM}/net" net && rm -rf "linux-${UPSTREAM}" linux.tar.xz
fi
cd net/tipc
cp "$HERE/tipc_devmap.h" .
python3 "$HERE/patch_bearer.py"
cat > Kbuild <<'KB'
obj-m := tipc.o
tipc-y += addr.o bcast.o bearer.o core.o link.o discover.o msg.o \
          name_distr.o subscr.o monitor.o name_table.o net.o \
          netlink.o netlink_compat.o node.o socket.o eth_media.o \
          topsrv.o group.o trace.o sysctl.o
CFLAGS_trace.o += -I$(src)
KB
make -C "/lib/modules/${KVER}/build" M="$PWD" modules
sudo mkdir -p "/lib/modules/${KVER}/extra"
sudo cp tipc.ko "/lib/modules/${KVER}/extra/"
sudo depmod -a
sudo modprobe tipc
echo tipc | sudo tee /etc/modules-load.d/tipc.conf >/dev/null
lsmod | grep ^tipc && echo "tipc.ko installed + loads on boot"
