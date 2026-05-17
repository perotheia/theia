# pero_theia — PERO CMP Workspace

Multi-repo manifest for the PERO CMP signal capture / gateway system.
Uses [Google repo tool](https://gerrit.googlesource.com/git-repo) for workspace management
and [Bazel](https://bazel.build) as the unified build system.

## Repos in this workspace

| Path | Description |
|---|---|
| `pero_cmp_ti/` | Hercules TMS570 capture firmware (ASAM-CMP over UDP) |
| `pero_cmp_ti_gw/` | Hercules gateway firmware (adds UDP RX / CAN+FR TX) |
| `pero_cmp_lnx/` | Linux host library (`libcmpdecoder`, `libgw`) + demo tools |
| `pero_cmp_gw_svc/` | Linux gateway NIF service (`cmp_gw`) |
| `pero_cmp_gw_cln_demo/` | TIPC gateway client demo (`cmp_gw_client`) |
| `mlbevo_gen2_cmp_psp/` | Platform support package (`libpsp.so`, `libcodec.a`) |
| `mlbevo_gen2_cmp_demo/` | Demo application using PSP |

## Prerequisites

```sh
# ARM GCC 7-2017-q4 (CCS 8 era) — for Hercules firmware cross-compile
sudo mkdir -p /opt/gcc-arm-none-eabi-7-2017-q4
curl -fL https://developer.arm.com/-/media/Files/downloads/gnu-rm/7-2017q4/gcc-arm-none-eabi-7-2017-q4-major-linux.tar.bz2 \
  | sudo tar -xj --strip-components=1 -C /opt/gcc-arm-none-eabi-7-2017-q4

# Host build dependencies
sudo apt install libpcap-dev libexpat1-dev gcc g++ cmake

# Bazelisk (downloads Bazel 9.1.0 automatically)
mkdir -p ~/.local/bin
curl -fsSL https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64 \
  -o ~/.local/bin/bazel && chmod a+x ~/.local/bin/bazel

# repo tool
curl -fsSL https://storage.googleapis.com/git-repo-downloads/repo \
  -o ~/.local/bin/repo && chmod a+x ~/.local/bin/repo
```

## Quick start

```sh
# Clone workspace
mkdir pero_workspace && cd pero_workspace
repo init -u git@cicd.skyway.porsche.com:PG50/pero_theia.git -b main
repo sync -j8

# Build host tools
bazel build //pero_cmp_lnx/demo:all --config=linux

# Cross-compile Hercules firmware (requires arm-none-eabi-gcc)
bazel build //pero_cmp_ti:pero_cmp_ti.elf --config=cortex_r4f

# Generate PSP + build
cd mlbevo_gen2_cmp_psp && bash generate.sh && cd ..
bazel build //mlbevo_gen2_cmp_psp:psp --config=linux
```

## Branch convention

| Branch | Purpose |
|---|---|
| `main` | Stable, tested |
| `discovery` | Active integration |
| `gateway` | Gateway feature work |
| `bz-migration` | Bazel build files + CI |

## Toolchain

The ARM cross-compiler for Hercules firmware is GCC 7-2017-q4 — the exact version
bundled with TI Code Composer Studio 8.x. It targets:

- CPU: Cortex-R4F
- FPU: VFPv3-D16 (hard float)
- Endian: **big-endian** (TMS570 default)
- ISA: Thumb-2

Host tools build with the system GCC (Ubuntu 24.04 default: GCC 13).
