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
  .bazelrc                       Build configs (--config=linux / ti_arm_cgt_18)
  toolchains/                    Cross-compile toolchain configs (TI armcl)
  platforms/                     Platform constraint definitions (tms570lc43xx etc.)
  rules/                         Custom Starlark rules (psp.bzl, opkg.bzl)
  packaging/                     opkg .ipk package definitions

gateway/
  firmware/pero_cmp_ti/        Hercules TMS570 capture firmware (ASAM-CMP over UDP)
  firmware/pero_cmp_ti_gw/     Hercules gateway firmware (CAN+FlexRay TX via UDP)
  libs/pero_cmp_lnx/           ASAM-CMP wire decode, FIBEX/DBC, PSP loader, timesync
  libs/libgw/                  Gateway NIF (CMP → PSP → TIPC + UDP TX)
  demo/pero_cmp_gw_cln_demo/   TIPC gateway client demo
  system/                      Gateway Artheia system fragment (pero_theia)
services/
  pero_cmp_gw_svc/             Linux gateway NIF service (cmp_gw binary)
applications/                  Vendor apps (e.g. odd_path_client)
autosar/
  mlbevo_gen2_cmp_psp/         Platform support package (codec + PSP + GwBusId)
  demo/mlbevo_gen2_cmp_demo/   ACC_07 pcap decoder demo
platform/
  system/                      Composition + symlinks to system fragments
  config/                      netgraph.cfg + host_netgraph.json
  runtime/                     Host-runtime abstraction (Lifecycle/Logger/Clock/Timer)
vendor/                        Per-vendor system fragments (Tornado, odd_path_client)
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

# 4. Python code generators — venv at workspace root
#    All generators (gen_platform_protos.py, gen_psp_registry.py, fibex_to_nanopb.py,
#    can_to_nanopb.py, etc.) are invoked from Bazel rules as `python3`, so the venv
#    must be on PATH for the build.
python3 -m venv .venv
./.venv/bin/pip install -e 'artheia[lsp,dev]'
#    Then prefix every bazel invocation:  PATH="$PWD/.venv/bin:$PATH" bazel build ...

# 5. TI ARM CGT 18.1.1.LTS — required for Hercules firmware
#    CCS 8.0.0 project: codegenToolVersion="18.1.1.LTS" (armcl, not GCC)
#    Download from: https://www.ti.com/tool/ARM-CGT  (TI account required)
#    Install to: /opt/ti/cgt_arm_18.1.1.LTS/
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

# 3. Make the venv's python3 visible to Bazel rules (PSP codegen, proto codegen, etc.)
export PATH="$PWD/.venv/bin:$PATH"

# 4. Build Linux host targets
bazel build //gateway/libs/pero_cmp_lnx/demo:all          # pero-decode, pero-filter, pero-timesync
bazel build //services/pero_cmp_gw_svc:cmp_gw
bazel build //gateway/demo/pero_cmp_gw_cln_demo:cmp_gw_client
bazel build //autosar/mlbevo_gen2_cmp_psp:codec    # PSP codegen + compile (~6000 .c files)
bazel build //autosar/mlbevo_gen2_cmp_psp:psp_so   # libpsp.so

# 5. Compile Hercules firmware (requires /opt/ti/cgt_arm_18.1.1.LTS/)
bazel build //gateway/firmware/pero_cmp_ti:pero_cmp_ti.elf    --config=ti_arm_cgt_18
bazel build //gateway/firmware/pero_cmp_ti_gw:pero_cmp_ti_gw.elf --config=ti_arm_cgt_18
```

## Verified build (theia workspace, bz-migration, 2026-05-18)

All targets build from a clean `repo sync` + `repo forall -c 'git checkout bz-migration'`
with no manual pre-steps.

| Target | Output | Config |
|---|---|---|
| `//gateway/libs/pero_cmp_lnx/lib:cmpdecoder` | `libcmpdecoder.so/.a` | host |
| `//gateway/libs/libgw:gw` | `libgw.so/.a` | host |
| `//gateway/libs/pero_cmp_lnx/demo:pero-decode` | `pero-decode` | host |
| `//gateway/libs/pero_cmp_lnx/demo:pero-filter` | `pero-filter` | host |
| `//gateway/libs/pero_cmp_lnx/demo:pero-timesync` | `pero-timesync` | host |
| `//services/pero_cmp_gw_svc:cmp_gw` | `cmp_gw` | host |
| `//gateway/demo/pero_cmp_gw_cln_demo:cmp_gw_client` | `cmp_gw_client` | host |
| `//autosar/mlbevo_gen2_cmp_psp:codec` | `codec.a` (5948 .c files) | host |
| `//autosar/mlbevo_gen2_cmp_psp:psp` | `psp.a` | host |
| `//autosar/mlbevo_gen2_cmp_psp:psp_so` | `libpsp.so` (2.3 MB) | host |
| `//autosar/demo/mlbevo_gen2_cmp_demo:mlbevo_demo` | `mlbevo_demo` | host |
| `//packaging:pero-libcmpdecoder` … `:pero-gw-stack` | `.ipk` packages | host |
| `//gateway/firmware/pero_cmp_ti:pero_cmp_ti.elf` | 975 KB ELF | `ti_arm_cgt_18` |
| `//gateway/firmware/pero_cmp_ti_gw:pero_cmp_ti_gw.elf` | ELF | `ti_arm_cgt_18` |

## PSP code generation (replaces generate.sh)

The `psp_generate` and `psp_library` Starlark rules in `//rules:psp.bzl`
replace `mlbevo_gen2_cmp_psp/generate.sh` entirely.

**Dependency tracking:** Bazel records the FIBEX XML and all 8 DBC files as
action inputs. Modifying any DBC file automatically invalidates the cache
and re-runs generation + compilation on the next `bazel build`.

```sh
# Generate sources only (no compile)
bazel build //autosar/mlbevo_gen2_cmp_psp:generate

# Full codec library (generates + compiles ~6000 .c files)
bazel build //autosar/mlbevo_gen2_cmp_psp:codec

# Verify DBC dependency: touch a DBC, then rebuild
touch platforms/mlbevo_gen2_cmp_psp/config/dbc/MLBevo_Gen2_MLBevo_KCAN_KMatrix_V8.27.01F.dbc
bazel build //autosar/mlbevo_gen2_cmp_psp:codec   # PspGenerate + PspCompile will re-run
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

# Generate dist.sh installer script then run it
bazel build //packaging:install
bash bazel-bin/packaging/dist.sh /opt/pero-gw/dist

# Install on target using opkg
opkg install /opt/pero-gw/dist/pero-gw-stack_1.0.0_x86_64.ipk \
             --add-dest root:/opt/pero-gw \
             --lists-dir /opt/pero-gw/dist
```

## Branch convention

| Branch | Purpose |
|---|---|
| `main` | Stable, tested |
| `discovery` | Active integration |
| `gateway` | Gateway feature work |
| `bz-migration` | Bazel BUILD files + CI pipeline |

## Toolchains

### Linux host (default)
System GCC 13 (Ubuntu 24.04). No extra install. Used for all
`//gateway/libs/pero_cmp_lnx/...`, `//services/...`, `//autosar/...` targets.

### TI ARM CGT 18.1.1.LTS (`--config=ti_arm_cgt_18`)
Original TI proprietary compiler used in the CCS 8.0.0 project files
(`codegenToolVersion = "18.1.1.LTS"`, i.e. `armcl` — not GCC).

- Install: `/opt/ti/cgt_arm_18.1.1.LTS/`  (requires TI account)
- Download: https://www.ti.com/tool/ARM-CGT
- Toolchain config: `toolchains/ti_arm_cgt_18/`
- Wrapper: `toolchains/ti_arm_cgt_18/armcl_wrapper.sh` — filters GCC-specific
  Bazel flags and adapts the invocation for armcl (response files, `-o`→`--output_file`, etc.)
- Linker: TI `.cmd` format (`source/generated_code/HL_sys_link.cmd`)
- Runs **without sandbox** (`--strategy=CppCompile=local`) so armcl can access
  its own runtime headers at `/opt/ti/cgt_arm_18.1.1.LTS/include/`

## CI pipeline (`.gitlab-ci.yml`)

Stages: `setup → build_host → build_firmware → test → package`

- `setup`: caches TI ARM CGT toolchain
- `build_host`: `bazel build //gateway/libs/pero_cmp_lnx/... //autosar/mlbevo_gen2_cmp_psp:codec`
- `build_firmware`: `bazel build //gateway/firmware/pero_cmp_ti:pero_cmp_ti.elf --config=ti_arm_cgt_18`
- `test`: `bazel test //gateway/libs/pero_cmp_lnx/...`
- `package`: `bazel build //packaging:install` — produces `.ipk` artifacts on main/tags
