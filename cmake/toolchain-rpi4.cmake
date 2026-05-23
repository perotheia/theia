# cmake/toolchain-rpi4.cmake — cross-compile to Raspberry Pi 4 (aarch64).
#
# Usage:
#   cmake -S platform/supervisor -B platform/supervisor/build-rpi4 \
#         -DCMAKE_TOOLCHAIN_FILE=$(pwd)/cmake/toolchain-rpi4.cmake
#   cmake --build platform/supervisor/build-rpi4 -j
#
# Prerequisites:
#   - gcc-aarch64-linux-gnu / g++-aarch64-linux-gnu (apt)
#   - Bookworm aarch64 sysroot at third_party/sysroot/rpi4/ — bootstrap
#     it with third_party/sysroot/setup_rpi4.sh.
#   - qemu-user-static for optional on-host functional tests
#     (qemu-aarch64-static -L <sysroot> ./binary).
#
# Override the sysroot path with -DRPI4_SYSROOT=/elsewhere or set
# the THEIA_RPI4_SYSROOT env var before invoking cmake.

# --- target identity ------------------------------------------------

set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# --- compiler ------------------------------------------------------

set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# --- sysroot resolution --------------------------------------------
# Priority:
#   1. -DRPI4_SYSROOT=... on the cmake command line
#   2. $THEIA_RPI4_SYSROOT in the environment
#   3. third_party/sysroot/rpi4/ relative to this file
# Bail loudly if none of them exists so the user gets a useful error
# instead of mysterious linker failures.

if(NOT RPI4_SYSROOT AND DEFINED ENV{THEIA_RPI4_SYSROOT})
    set(RPI4_SYSROOT "$ENV{THEIA_RPI4_SYSROOT}")
endif()

if(NOT RPI4_SYSROOT)
    get_filename_component(_workspace "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
    set(RPI4_SYSROOT "${_workspace}/third_party/sysroot/rpi4")
endif()

if(NOT EXISTS "${RPI4_SYSROOT}/usr/lib/aarch64-linux-gnu")
    message(FATAL_ERROR
        "RPi4 sysroot not found at ${RPI4_SYSROOT}.\n"
        "Bootstrap it with third_party/sysroot/setup_rpi4.sh "
        "or pass -DRPI4_SYSROOT=/path/to/sysroot.")
endif()

set(CMAKE_SYSROOT "${RPI4_SYSROOT}")

# --- search-path policy --------------------------------------------
# Compiler comes from the host (NEVER search the sysroot for it), but
# libraries/headers/programs that the build consumes MUST come from
# the sysroot. The find_package() pkg-config probe in particular gets
# confused without these.

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config: point it at the sysroot's .pc files (used by
# pkg_check_modules() in platform/supervisor/CMakeLists.txt for
# yaml-cpp + protobuf). PKG_CONFIG_LIBDIR overrides the host's
# default search path entirely — without this, pkg-config will happily
# hand back amd64 .pc files and the link silently picks up host libs.

set(ENV{PKG_CONFIG_DIR} "")
set(ENV{PKG_CONFIG_LIBDIR}
    "${RPI4_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:${RPI4_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${RPI4_SYSROOT}")

# --- linker hints --------------------------------------------------
# rpath-link points the *link-time* loader at the sysroot's
# transitive .so deps (e.g. libgrpc++ pulls in dozens of libabsl_*
# that the user's link line never mentions). Without this the link
# fails with "cannot find -labsl_strings" type errors.

set(_lib "${RPI4_SYSROOT}/usr/lib/aarch64-linux-gnu")
add_link_options(
    "-Wl,-rpath-link=${_lib}"
    "-L${_lib}"
)

# --- protoc / artheia bypass for cross builds ----------------------
# platform/supervisor/CMakeLists.txt re-runs `artheia gen-proto` at
# configure time and links against host `protoc` for the .proto
# compilation. Both are correctly amd64 (build-time tools, not
# target-arch). The libprotobuf .so it ends up linking against IS
# target-arch via the sysroot, so this is fine — but the auto-gen
# step expects host protoc, not a cross protoc, so leave it as-is.

# Output ID — distinguishes the build dir's binary in logs.
message(STATUS "toolchain-rpi4: sysroot = ${RPI4_SYSROOT}")
message(STATUS "toolchain-rpi4: compiler = ${CMAKE_CXX_COMPILER}")
