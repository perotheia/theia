# Cross-compile Theia to Raspberry Pi 4 (aarch64)

Goal: produce per-machine deploy bundles for a Raspberry Pi 4 target —
the same `bazel build @rig_<name>//<machine>:image` pipeline that
emits an `amd64` `.ipk` today should also emit an `arm64` `.ipk`
installable on a Pi 4 running 64-bit Raspberry Pi OS (Debian-based).

This is the first non-x86 Linux target after the docker compose
host. The Hercules firmware already cross-compiles (TI ARM-CGT), but
that's bare-metal — separate toolchain, no shared object pipeline.

## Why

- **Customer hardware**: the gateway BU + a couple of dev rigs run
  on Pi 4 class boards; we currently hand-build there. A first-class
  cross-compile fold collapses ~30 min of manual setup per board.
- **Bazel hygiene**: forces us to express toolchain selection as a
  `--platforms=//rules/config:rpi4` flag rather than implicit
  host-toolchain. Catches a class of "works on my machine" bugs.
- **services-com on arm64**: today's gRPC bridge binary is amd64-only.
  A real cross-compile validates the full stack (TIPC, gRPC,
  protobuf, abseil) on a non-x86 target.

## Pieces

1. **Bazel platform + toolchain**
   - Define `//rules/config:rpi4` platform constraint
     (`cpu=aarch64, os=linux`).
   - Pick a cross-toolchain. Two realistic options:
     - `aarch64-linux-gnu-gcc` from `gcc-aarch64-linux-gnu` apt package
       on the workspace host (simplest, ABI-compatible with Debian
       bookworm aarch64).
     - A hermetic toolchain from `bazel-contrib/toolchains_llvm` or
       `grailbio/bazel-toolchain` (cleaner but more setup).
   - Recommend gcc-aarch64-linux-gnu for the first cut — matches what
     the customer's Pi runs, ABI-stable, no extra Bazel deps.

2. **Sysroot for runtime libs** — DONE. `third_party/sysroot/rpi4/`
   is a 444 MB Debian bookworm aarch64 rootfs (minbase + libyaml-cpp-dev
   libprotobuf-dev libgrpc++-dev libgrpc-dev libabsl-dev). Created by
   `third_party/sysroot/setup_rpi4.sh` (gitignored; the script is
   committed). Sysroot lib versions:
   - `libyaml-cpp.so.0.7` (same major as host, but bookworm's)
   - `libprotobuf.so.32` (host has 23; bookworm bumped this)
   - `libgrpc++.so.1.51` / `libgrpc.so.27`
   - `libabsl_strings.so.20220623` (host has 20210324)
   These are the sonames the cross-built binaries will request at
   runtime on the Pi — fine because Pi 4 + Raspberry Pi OS 12 = same
   bookworm libs.

   Verified end-to-end with a yaml-cpp smoke (cross-link + qemu-aarch64-static
   run) — yaml parses correctly under emulation.

3. **Update `pkg_opkg` arch**
   - `rules/rig.bzl` currently hardcodes `arch = "amd64"` (see
     `_machine_targets`). It needs to read the target platform:
     `arch = "arm64"` when `//rules/config:rpi4` is selected, else `amd64`.
   - The Bazel `select()` on `@platforms//cpu:arm64` returns the
     right value. The .ipk name (`demo-<machine>_1.0.0_arm64.ipk`)
     should also reflect the arch.

4. **Per-machine rig.py mapping**
   - `demo/manifest/rig.py`'s `ComputeHost` is declared `aarch64`
     already (the rig metadata, not just amd64 hardcoded). Verify
     it's wired through to Bazel platform selection — today the
     pkg_opkg arch is hardcoded regardless of rig.py.

5. **Cross-compile services-com + supervisor**
   - Both are CMake projects (`platform/supervisor/`,
     `services/com/`), NOT Bazel. Need either:
     - **Path A**: keep them as CMake; add `toolchain-rpi4.cmake`
       and a `build-rpi4/` out-of-tree dir. CI/dev does
       `cmake -S . -B build-rpi4 -DCMAKE_TOOLCHAIN_FILE=...`.
     - **Path B**: migrate them into Bazel so the rig-level platform
       selection drives them too. Bigger lift.
   - Recommend Path A (cmake toolchain file) for the first cut.

6. **TIPC on Pi 4**
   - Sanity-check: `modprobe tipc` on the Pi 4's kernel. Mainline
     Raspberry Pi OS 12 (bookworm) ships TIPC as a module — no
     custom kernel build needed. Worth a one-line verification
     before committing to the architecture.

7. **Deploy mechanism**
   - The docker compose path doesn't change; the Pi gets the .ipk
     pushed by `scp` + `dpkg -i` (Pi runs Debian, so dpkg works the
     same way the container's dpkg path does — we already de-risked
     this when switching from opkg to dpkg).
   - Add `tools/deploy_rpi4.sh` that wraps:
     ```
     bazel build @rig_<n>//<machine>:image --platforms=//rules/config:rpi4
     scp bazel-bin/.../<machine>.ipk pi@<host>:/tmp/
     ssh pi@<host> 'sudo dpkg -i /tmp/<machine>.ipk'
     ```

## Out of scope (this task)

- Pi Zero / Pi 3 (armv6/v7) — separate platform constraint, separate
  sysroot. Add later if a customer asks.
- Pi RP2040 (Cortex-M0+) — bare-metal like Hercules; would need its
  own TI-style toolchain integration, not the Linux pipeline.
- Yocto/buildroot images — we deploy to the customer's existing
  Pi OS rootfs, not a Theia-owned image.

## Order of work (suggested)

1. ~~Install `gcc-aarch64-linux-gnu` on the workspace host; verify
   `aarch64-linux-gnu-gcc --version` works (no Bazel involvement).~~
   **DONE** — `gcc-aarch64-linux-gnu 11.4.0` + `g++-aarch64-linux-gnu`
   installed via apt. Hello-world cross-compile produces valid
   `ARM aarch64` ELF for both C and C++. `qemu-user-static` also
   installed so the binaries run on the workspace host for
   functional testing without a physical Pi (`qemu-aarch64-static
   ./binary` works).
2. ~~Debootstrap a minimal aarch64 sysroot under
   `third_party/sysroot/rpi4/` with libyaml-cpp / libprotobuf /
   libgrpc++ / libabsl + their `-dev`.~~ **DONE** — `setup_rpi4.sh`
   bootstraps it; sysroot itself is gitignored. End-to-end smoke
   (cross-link + qemu-aarch64-static run) verified.
3. ~~Add `cmake/toolchain-rpi4.cmake` and cross-build the supervisor
   binary. Manual scp to a Pi 4 and verify it runs (without the .ipk
   pipeline — just `./supervisor --help`). This de-risks the
   sysroot before touching Bazel.~~ **DONE** (host-side, qemu-tested).
   Toolchain file lives at `cmake/toolchain-rpi4.cmake`; wrappers
   `cmake/protoc-rpi4.sh` + `cmake/grpc-cpp-plugin-rpi4.sh` route
   protoc/grpc_cpp_plugin through qemu so the sysroot's 3.21 protoc
   (matching libprotobuf 32) regenerates .pb.{h,cc} during the
   cross-build. supervisor binary verified via `qemu-aarch64-static
   -L $sysroot ./supervisor --help`.

   Pre-existing latent bug fixed along the way: services/com/CMakeLists.txt
   was missing `ThreadSample` from SUP_PROTOS (added when ChildState
   gained ThreadSample in task #225). Host amd64 build masked it
   because protoc 3.12 reuses the supervisor's already-built .pb.h
   from a sibling target.

4. ~~Cross-build services-com the same way. Verify TIPC connect on the
   Pi 4 (start supervisor → start services-com → curl/grpcurl from
   another machine).~~ **CROSS-BUILD DONE** (host-side, qemu-tested).
   services-com binary at `services/com/build-rpi4/services-com` (611 KB
   aarch64 ELF). `--help` runs cleanly under qemu. The TIPC-connect
   smoke needs a real Pi 4 (qemu-user-static doesn't emulate AF_TIPC
   sockets reliably); deferred to step 7.
5. ~~Wire up Bazel `//rules/config:rpi4` platform + arch selection in
   `rules/rig.bzl`.~~ **DONE.** `//rules/config:BUILD.bazel` declares both
   `host` (linux + x86_64) and `rpi4` (linux + aarch64) platforms,
   plus `is_amd64`/`is_arm64` config_settings. `rules/rig.bzl`'s
   `pkg_opkg(arch=...)` is now a `select()` keyed on those settings.
   Verified all three paths:
   - Default (no flag) → `demo-compute_host_1.0.0_amd64.ipk`
   - `--platforms=//rules/config:host` → same amd64
   - `--platforms=//rules/config:rpi4` → `demo-compute_host_1.0.0_arm64.ipk`
   Control file's `Architecture:` field tracks; ipk name reflects arch.

   Note: today's compute_host components are `bazel_buildable=False`
   placeholder shell scripts, so the produced .ipk is arch-agnostic
   content with arch-tagged metadata. When the demo apps + FCs grow
   real cc_binary targets, Bazel's cc toolchain selection will need
   the same `//rules/config:rpi4` wiring (separate piece of work — see
   step 6 below).
6. ~~Bazel `pkg_opkg(arch=...)` reads from rig.py.~~ **DONE** —
   `rules/rig.bzl`'s `_machine_targets()` now emits `arch = "<rig_arch>"`
   from the rig-deps JSON's `arch` field. The previous `select()` only
   fired when an explicit `--platforms=//rules/config:rpi4` was passed; the
   rule now uses the rig-declared arch as the source of truth and
   `--platforms=` is reserved for cc_toolchain selection (relevant
   once components grow real cc_binary targets).

   Verified:
   - `bazel build @rig_demo//compute_host:image` (no flag) →
     `demo-compute_host_1.0.0_arm64.ipk` (ComputeHost declared aarch64
     in rig.py)
   - `bazel build @rig_demo//central_host:image` (no flag) →
     `demo-central_host_1.0.0_amd64.ipk` (currently broken by an
     unrelated `:ipk` target rename — pre-existing, not part of this
     task)

7. ~~`tools/deploy_rpi4.sh`.~~ **DONE** — wraps the canonical workflow:
   - TIPC modprobe preflight on the target Pi (covers step 6's
     "modprobe tipc" sanity check) — refuses to deploy if AF_TIPC is
     unavailable in the kernel.
   - `bazel build @rig_<name>//<machine>:image --platforms=//rules/config:rpi4`.
   - `bazel cquery --output=files` to locate the produced .ipk
     (no hardcoded paths).
   - `scp` + `ssh sudo dpkg -i`.
   - Sanity-warns if the .ipk's arch tag isn't `_arm64`.

   Real-hardware verification (compose `dpkg -i` + `supdbg --target
   pi:7700 tree`) deferred — the workspace doesn't currently have a
   Pi 4 attached. The script's preflight + scp/dpkg chain is the
   complete deploy path once hardware is on the bench.

## Status

Steps 1-5 + 6 (arch wiring) + 7 (deploy script) are landed. The
remaining work — real-Pi-4 dpkg + supdbg smoke — is gated on
hardware, not on code. When that happens, the task moves to DONE/.

## Related

- `docs/tasks/DONE/` will record the per-phase commits.
- `deploy/docker-compose.yml` stays amd64-only; the Pi 4 path is
  out-of-container deploy.
- supdbg already works against any reachable services-com regardless
  of arch — the gRPC wire format is the same, so step 6's validation
  is "supdbg --target pi:7700 tree" returning a tree.
