#!/usr/bin/env bash
# tools/rpi4_tipc_kernel.sh — build + deploy a TIPC-enabled kernel to a Pi 4.
#
# WHY: Raspberry Pi OS 13 (trixie) ships the rpt 6.12 kernel with CONFIG_TIPC NOT
# set, and net_device has no `tipc_ptr` field — so an out-of-tree tipc.ko cannot
# even compile against it. The theia runtime transport is TIPC (AF_TIPC sockets),
# so the Pi needs a kernel built with CONFIG_TIPC=m. This cross-builds that kernel
# on a dev box and installs it side-by-side on the Pi (stock kernel kept as a
# fallback), then loads tipc. See docs/tasks/PROGRESS/WIFI.md / VPN.md.
#
# Verified: built 6.12.93-v8+ (rpi-6.12.y) with CONFIG_TIPC=m from the Pi's own
# .config (olddefconfig), booted it on a Pi 4 (192.168.50.213), `modprobe tipc`
# + AF_TIPC SOCK_SEQPACKET OK, and ran services/nm live over real TIPC + wlan0
# (the FSM walked NETWORK_OFF→…→WIFI_ASSOCIATED→…→NETWORK_OPERATIONAL).
#
# Usage:
#   tools/rpi4_tipc_kernel.sh <pi-ssh-host>          # full: fetch+build+deploy
#   tools/rpi4_tipc_kernel.sh <pi-ssh-host> deploy   # deploy an already-built tree
#
# Requires on the dev box: gcc-aarch64-linux-gnu, bison, flex, bc, libssl-dev,
# libelf-dev, git. Passwordless ssh + sudo to the Pi.
set -euo pipefail

PI="${1:?usage: rpi4_tipc_kernel.sh <pi-ssh-host> [deploy]}"
STAGE_ONLY="${2:-}"
WORK="${RPI_KERNEL_WORK:-/tmp/pikernel}"
SRC="$WORK/rpt"
BRANCH="${RPI_KERNEL_BRANCH:-rpi-6.12.y}"   # match the Pi's running 6.12.x line
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KERNEL=kernel8

mkdir -p "$WORK"

if [ "$STAGE_ONLY" != "deploy" ]; then
    echo "==> fetch the Pi's running .config (authoritative base — only TIPC is flipped)"
    scp "$PI:/lib/modules/\$(uname -r)/build/.config" "$WORK/pi.config" 2>/dev/null \
        || ssh "$PI" 'zcat /proc/config.gz' > "$WORK/pi.config"

    echo "==> clone rpt kernel source ($BRANCH)"
    [ -d "$SRC/.git" ] || git clone --depth 1 --branch "$BRANCH" \
        https://github.com/raspberrypi/linux.git "$SRC"
    cd "$SRC"

    echo "==> base config = Pi's config + CONFIG_TIPC=m"
    cp "$WORK/pi.config" .config
    ./scripts/config --module TIPC \
                     --enable TIPC_MEDIA_UDP \
                     --enable TIPC_CRYPTO \
                     --module TIPC_DIAG
    make olddefconfig
    grep -q '^CONFIG_TIPC=m' .config || { echo "TIPC not set after olddefconfig"; exit 1; }

    echo "==> build Image + modules + dtbs (-j$(nproc))"
    make -j"$(nproc)" Image modules dtbs
    [ -f net/tipc/tipc.ko ] || { echo "tipc.ko did not build"; exit 1; }
fi

cd "$SRC"
REL=$(make kernelrelease 2>/dev/null | tail -1)
echo "==> kernel release: $REL  (installs side-by-side; stock kernel kept)"

echo "==> stage + ship modules (own /lib/modules/$REL dir — never clobbers stock)"
MODSTAGE="$WORK/modstage"; rm -rf "$MODSTAGE"; mkdir -p "$MODSTAGE"
make -j"$(nproc)" INSTALL_MOD_PATH="$MODSTAGE" modules_install >/dev/null
rsync -a "$MODSTAGE/lib/modules/$REL" "$PI:/tmp/newmods/"
ssh "$PI" "sudo rsync -a /tmp/newmods/$REL /lib/modules/ && sudo depmod $REL && echo '   modules + depmod ok'"

echo "==> ship Image + Pi-4 dtb; back up the stock kernel ONCE"
scp arch/arm64/boot/Image "$PI:/tmp/kernel8-tipc.img"
scp arch/arm64/boot/dts/broadcom/bcm2711-rpi-4-b.dtb "$PI:/tmp/bcm2711-rpi-4-b.dtb.new"
ssh "$PI" bash -s <<'REMOTE'
set -euo pipefail
FW=/boot/firmware
[ -f "$FW/kernel8.img.pre-tipc.bak" ] || { sudo cp -a "$FW/kernel8.img" "$FW/kernel8.img.pre-tipc.bak"; echo "   stock kernel -> kernel8.img.pre-tipc.bak"; }
sudo cp /tmp/kernel8-tipc.img "$FW/kernel8-tipc.img"
sudo cp -a "$FW/bcm2711-rpi-4-b.dtb" "$FW/bcm2711-rpi-4-b.dtb.pre-tipc.bak" 2>/dev/null || true
sudo cp /tmp/bcm2711-rpi-4-b.dtb.new "$FW/bcm2711-rpi-4-b.dtb"
grep -q '^kernel=kernel8-tipc.img' "$FW/config.txt" || echo 'kernel=kernel8-tipc.img' | sudo tee -a "$FW/config.txt" >/dev/null
echo tipc | sudo tee /etc/modules-load.d/tipc.conf >/dev/null   # auto-load at boot
echo "   deployed; reboot to boot the TIPC kernel"
REMOTE

cat <<EOF

==> Done. Now:
     ssh $PI sudo reboot
     # after it returns:
     ssh $PI 'uname -r; sudo modprobe tipc; lsmod | grep tipc'
   Recovery if it won't boot: from another machine, edit the SD card's
   firmware partition and set kernel=kernel8.img.pre-tipc.bak (or remove the
   kernel= line) to fall back to the stock kernel.
EOF
