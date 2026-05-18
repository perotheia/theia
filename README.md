# pero_theia — PERO CMP Workspace (Bazel files)

Bazel workspace for the PERO CMP signal capture / gateway system.
Uses **Google repo tool** for multi-repo management and **Bazel 9** as the
unified build system. `generate.sh` has been replaced entirely by Bazel rules.

> **This repo contains the workspace files** (MODULE.bazel, BUILD rules,
> toolchains, platforms, packaging).  
> The repo manifest lives in a separate repo:
> [`pero_theia_manifest`](https://cicd.skyway.porsche.com/PG50/pero_theia_manifest)

## Workspace layout

After `repo sync`, the workspace looks like:

```
.                              ← pero_theia (this repo — Bazel workspace root)
  MODULE.bazel                   Bazel module + dep declarations
  .bazelrc                       Build configs (--config=linux / cortex_r4f / ti_arm_cgt_18)
  toolchains/                    Cross-compile toolchain configs (GCC + TI armcl)
  platforms/                     Platform constraint definitions (tms570lc43xx etc.)
  rules/                         Custom Starlark rules (psp.bzl, opkg.bzl)
  packaging/                     opkg .ipk package definitions

gateway/
  pero_cmp_ti/                 Hercules TMS570 capture firmware (ASAM-CMP over UDP)
  pero_cmp_ti_gw/              Hercules gateway firmware (CAN+FlexRay TX via UDP)
  pero_cmp_lnx/                Linux host library (libcmpdecoder, libgw) + demo tools
services/
  pero_cmp_gw_svc/             Linux gateway NIF service (cmp_gw binary)
applications/
  pero_cmp_gw_cln_demo/        TIPC gateway client demo (cmp_gw_client binary)
platforms/
  mlbevo_gen2_cmp_psp/         Platform support package (codec + PSP + GwBusId)
  mlbevo_gen2_cmp_demo/        ACC_07 pcap decoder demo
```

## Prerequisites

```sh
# 1. repo tool
mkdir -p ~/.local/bin
curl -fsSL https://storage.googleapis.com/git-repo-downloads/repo \
  -o ~/.local/bin/repo && chmod a+x ~/.local/bin/repo

# 2. Bazelisk (downloads Bazel 9.1.0 on first use)
curl -fsSL https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64 \
  -o ~/.local/bin/bazel && chmod a+x ~/.local/bin/bazel
export PATH=~/.local/bin:$PATH

# 3. Host build deps (Ubuntu 24.04)
sudo apt install libpcap-dev libexpat1-dev libnanopb-dev gcc g++

# 4. Python code-generator dep
pip install jinja2        # used by gen_platform_protos.py

# 5. ARM GCC 7-2017-q4 (CCS 8 era) — for Hercules firmware cross-compile
sudo mkdir -p /opt/gcc-arm-none-eabi-7-2017-q4
curl -fL https://developer.arm.com/-/media/Files/downloads/gnu-rm/7-2017q4/gcc-arm-none-eabi-7-2017-q4-major-linux.tar.bz2 \
  | sudo tar -xj --strip-components=1 -C /opt/gcc-arm-none-eabi-7-2017-q4

# 6. TI ARM CGT 18.1.1.LTS (original CCS 8 compiler, requires TI account)
#    Download from: https://www.ti.com/tool/ARM-CGT
#    Install to: /opt/ti/cgt_arm_18.1.1.LTS/
#    The project was originally built with: codegenToolVersion="18.1.1.LTS"
#    armcl toolchain config lives in: toolchains/ti_arm_cgt_18/
```

## Quick start

```sh
# 1. Bootstrap workspace from the manifest repo
mkdir theia && cd theia
repo init -u git@cicd.skyway.porsche.com:PG50/pero_theia_manifest.git \
          -b main --partial-clone --depth=1
repo sync -j8

# repo checks out pero_theia at workspace root (MODULE.bazel, toolchains/, etc.)
# and all component repos at their hierarchical paths.

# 2. Activate bz-migration on all component repos
repo forall -c 'git checkout bz-migration'

# 3. Build (no manual generate steps — Bazel handles PSP codegen)
bazel build //gateway/pero_cmp_lnx/lib:cmpdecoder
bazel build //gateway/pero_cmp_lnx/lib:gw
bazel build //gateway/pero_cmp_lnx/demo:all          # pero-decode, pero-filter, pero-timesync
bazel build //services/pero_cmp_gw_svc:cmp_gw
bazel build //applications/pero_cmp_gw_cln_demo:cmp_gw_client
bazel build //platforms/mlbevo_gen2_cmp_psp:codec    # PspGenerate + PspCompile (~6000 .c)
bazel build //platforms/mlbevo_gen2_cmp_psp:psp_so   # libpsp.so
bazel build //platforms/mlbevo_gen2_cmp_demo:mlbevo_demo

# Cross-compile Hercules firmware (requires arm-none-eabi-gcc-7-2017-q4)
bazel build //gateway/pero_cmp_ti:pero_cmp_ti.elf --config=cortex_r4f
```

## Verified build (theia workspace, bz-migration, 2026-05-18)

All targets build from a clean `repo sync` with no manual pre-steps.

| Target | Output | Status |
|---|---|---|
| `//gateway/pero_cmp_lnx/lib:cmpdecoder` | `libcmpdecoder.so` / `.a` | ✓ |
| `//gateway/pero_cmp_lnx/lib:gw` | `libgw.so` / `.a` | ✓ |
| `//gateway/pero_cmp_lnx/demo:pero-decode` | `pero-decode` | ✓ |
| `//gateway/pero_cmp_lnx/demo:pero-filter` | `pero-filter` | ✓ |
| `//gateway/pero_cmp_lnx/demo:pero-timesync` | `pero-timesync` | ✓ |
| `//services/pero_cmp_gw_svc:cmp_gw` | `cmp_gw` | ✓ |
| `//applications/pero_cmp_gw_cln_demo:cmp_gw_client` | `cmp_gw_client` | ✓ |
| `//platforms/mlbevo_gen2_cmp_psp:codec` | `codec.a` (5948 .c → .a) | ✓ |
| `//platforms/mlbevo_gen2_cmp_psp:psp` | `psp.a` | ✓ |
| `//platforms/mlbevo_gen2_cmp_psp:psp_so` | `libpsp.so` (2.3 MB) | ✓ |
| `//platforms/mlbevo_gen2_cmp_demo:mlbevo_demo` | `mlbevo_demo` | ✓ |
| `//packaging:pero-libcmpdecoder` | `.ipk` | ✓ |
| `//packaging:pero-libpsp` | `.ipk` | ✓ |
| `//packaging:pero-libgw` | `.ipk` | ✓ |
| `//packaging:pero-gw-svc` | `.ipk` | ✓ |
| `//packaging:pero-gw-client` | `.ipk` | ✓ |
| `//packaging:pero-gw-stack` | `.ipk` (meta) | ✓ |

Firmware cross-compile (`//gateway/pero_cmp_ti:pero_cmp_ti.elf --config=cortex_r4f`)
requires `/opt/gcc-arm-none-eabi-7-2017-q4/` — see Prerequisites.

## PSP code generation (replaces generate.sh)

The `psp_generate` and `psp_library` Starlark rules in `//rules:psp.bzl`
replace `mlbevo_gen2_cmp_psp/generate.sh` entirely.

**Dependency tracking:** Bazel records the FIBEX XML and all 8 DBC files as
action inputs. Modifying any DBC file automatically invalidates the cache
and re-runs generation + compilation on the next `bazel build`.

```sh
# Generate sources only (no compile) — writes to bazel-out tree
bazel build //mlbevo_gen2_cmp_psp:generate

# Full codec library (generates + compiles all ~6000 .c files)
bazel build //platforms/mlbevo_gen2_cmp_psp:codec

# Verify DBC dependency: touch a DBC, then rebuild
touch mlbevo_gen2_cmp_psp/config/dbc/MLBevo_Gen2_MLBevo_KCAN_KMatrix_V8.27.01F.dbc
bazel build //platforms/mlbevo_gen2_cmp_psp:codec   # PspGenerate + PspCompile will re-run
```

## Manifest repos

| Repo | Purpose |
|---|---|
| [`pero_theia_manifest`](https://cicd.skyway.porsche.com/PG50/pero_theia_manifest) | `default.xml` only — used with `repo init -u` |
| [`pero_theia`](https://cicd.skyway.porsche.com/PG50/pero_theia) | Bazel workspace files (this repo) — checked out at `.` |

### Local manifests

After `repo init`, create `.repo/local_manifests/` to add extra projects
or override paths without modifying the shared manifest:

```sh
mkdir -p .repo/local_manifests

# Example: add a private repo to the workspace
cat > .repo/local_manifests/private.xml << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<manifest>
  <remote name="private" fetch="git@my-server.example.com:my-org" />
  <project name="my-private-repo" path="private/my-repo" remote="private"
           revision="main" />
</manifest>
EOF

repo sync -j8   # picks up the new project automatically
```

## opkg packaging

The `//packaging` package produces installable `.ipk` files (opkg/OpenWrt
format — `ar` archive with `debian-binary`, `control.tar.gz`, `data.tar.gz`).
The rule is in `rules/opkg.bzl`.

### Packages

| Package | Contents | Install path | Depends |
|---|---|---|---|
| `pero-libcmpdecoder` | `libcmpdecoder.so` | `/usr/lib/` | — |
| `pero-libpsp` | `libpsp.so` | `/usr/lib/` | — |
| `pero-libgw` | `libgw.so` | `/usr/lib/` | libcmpdecoder, libpsp |
| `pero-gw-svc` | `cmp_gw` | `/usr/bin/` | libgw, libcmpdecoder, libpsp |
| `pero-gw-client` | `cmp_gw_client` | `/usr/bin/` | — |
| `pero-gw-firmware` | Hercules ELF | `/usr/share/pero-gw-fw/` | — |
| `pero-gw-stack` | meta-package | — | full runtime stack |

### Build and install

```sh
# Build all packages
bazel build //packaging:pero-libcmpdecoder \
            //packaging:pero-libpsp \
            //packaging:pero-libgw \
            //packaging:pero-gw-svc \
            //packaging:pero-gw-client \
            //packaging:pero-gw-stack

# Build Packages index (opkg repo metadata)
bazel build //packaging:Packages

# Generate dist.sh installer script then run it
bazel build //packaging:install
bash bazel-bin/packaging/dist.sh /opt/pero-gw/dist

# Install on target using opkg
opkg install /opt/pero-gw/dist/pero-gw-stack_1.0.0_x86_64.ipk \
             --add-dest root:/opt/pero-gw \
             --lists-dir /opt/pero-gw/dist
```

### Install layout after dist.sh

```
/opt/pero-gw/dist/
├── Packages.idx                         # opkg repository index
├── pero-libcmpdecoder_1.0.0_x86_64.ipk
├── pero-libpsp_1.0.0_x86_64.ipk        # 2.3 MB — MLBevo Gen2 codec
├── pero-libgw_1.0.0_x86_64.ipk
├── pero-gw-svc_1.0.0_x86_64.ipk
├── pero-gw-client_1.0.0_x86_64.ipk
├── pero-gw-firmware_1.0.0_all.ipk
└── pero-gw-stack_1.0.0_x86_64.ipk      # meta: installs full stack
```

### Adding a new package

```python
# In packaging/BUILD.bazel:
load("//rules:opkg.bzl", "pkg_opkg")

pkg_opkg(
    name        = "my-package",
    package     = "my-package",
    version     = "1.0.0",
    arch        = "x86_64",
    description = "Short description",
    depends     = "pero-libcmpdecoder",
    files = {
        "//my_repo:my_binary": "/usr/bin/my_binary",
        "//my_repo:my_lib_so_file": "/usr/lib/libmy.so",
    },
    postinst = "#!/bin/sh\nldconfig\n",
)
```

### opkg-build tool

`~/.local/bin/opkg-build` is a local script (opkg-utils is not in Ubuntu 24.04
apt). The Bazel rule builds packages directly with `ar` and `tar` — no
dependency on the opkg-build script at build time.

## Branch convention

| Branch | Purpose |
|---|---|
| `main` | Stable, tested |
| `discovery` | Active integration |
| `gateway` | Gateway feature work |
| `bz-migration` | Bazel BUILD files + CI pipeline |

## Toolchains

### Linux host (default)
System GCC 13 (Ubuntu 24.04). Used for all `//gateway/pero_cmp_lnx/...`,
`//pero_cmp_gw_svc/...`, `//mlbevo_gen2_cmp_psp/...` targets.

### ARM GCC 7-2017-q4 (`--config=cortex_r4f`)
Open-source ARM cross-compiler matching the CCS 8 era toolchain.
Install: `/opt/gcc-arm-none-eabi-7-2017-q4/`
Config: `toolchains/arm_gcc_7_2017q4/`

### TI ARM CGT 18.1.1.LTS (`--config=ti_arm_cgt_18`)
Original TI proprietary compiler used in CCS 8.0.0 project files.
`codegenToolVersion = "18.1.1.LTS"` (`armcl`, not GCC).
Requires manual download from https://www.ti.com/tool/ARM-CGT
Install: `/opt/ti/cgt_arm_18.1.1.LTS/`
Config: `toolchains/ti_arm_cgt_18/`
Linker: `.cmd` format (`source/generated_code/HL_sys_link.cmd`)

## CI pipeline (`.gitlab-ci.yml`)

Stages: `setup → build_host → build_firmware → test → package`

- `setup`: downloads arm-gcc toolchain, caches it
- `build_host`: `bazel build //gateway/pero_cmp_lnx/... //platforms/mlbevo_gen2_cmp_psp:codec`
- `build_firmware`: `bazel build //gateway/pero_cmp_ti:pero_cmp_ti.elf --config=cortex_r4f`
- `test`: `bazel test //gateway/pero_cmp_lnx/...`
- `package`: produces `psp_tar.tar.gz` artifact on main/tags

## Local development (symlink workspace)

Without running `repo sync`, you can build locally using symlinks:

```sh
cd /path/to/pero_theia
ln -s /path/to/ccstheia/pero_cmp_lnx .
ln -s /path/to/ccstheia/mlbevo_gen2_cmp_psp .
# ... etc for other repos
bazel build //...
```
