# third_party/sysroot/ — cross-compile sysroots

Bootstrapped Debian rootfs trees used as `--sysroot` when cross-compiling
Theia C++ binaries to non-x86 targets. The rootfs contents themselves
are gitignored (each is ~450 MB of .debs); the `setup_*.sh` scripts
recreate them on demand.

| Script | Target | Result |
|---|---|---|
| `setup_rpi4.sh` | Raspberry Pi 4 (aarch64, Debian 12 bookworm) | `rpi4/` (~444 MB) |

> Sysroot suite is **bookworm**, not the Pi's trixie: the host's gcc-11/12 cross
> toolchain can't resolve trixie's glibc-2.38 / GLIBCXX-3.4.32 symbols. bookworm
> matches gcc-11/12 and runs on trixie via glibc forward-compat. See
> `setup_rpi4.sh`'s header.

Each sysroot pulls in the lib*-dev packages Theia links against
(`libyaml-cpp-dev`, `libprotobuf-dev`, `libgrpc++-dev`, `libgrpc-dev`,
`libabsl-dev`).

## Usage

```bash
# One-time setup.
./third_party/sysroot/setup_rpi4.sh

# Cross-compile a single source file.
SR=$(pwd)/third_party/sysroot/rpi4
aarch64-linux-gnu-g++ --sysroot="$SR" \
    -I"$SR/usr/include" \
    -L"$SR/usr/lib/aarch64-linux-gnu" \
    -Wl,-rpath-link="$SR/usr/lib/aarch64-linux-gnu" \
    -o myprog src.cpp -lyaml-cpp

# Run on the host (no Pi needed).
qemu-aarch64-static -L "$SR" ./myprog
```

See `docs/tasks/BACKLOG/cross-compile-rpi4.md` for the multi-step
plan (sysroot is step 2 of 7; next is wiring CMake/Bazel toolchain
files).
