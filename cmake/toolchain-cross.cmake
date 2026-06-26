# cmake/toolchain-cross.cmake — the ONE generic Theia cross-compile toolchain.
#
# Replaces the per-target toolchain-<board>.cmake archaeology. Parameterized by
# the target registry (rules/config/targets.bzl) via env vars / -D, so a new board
# adds a registry entry, NOT a new cmake file:
#
#   THEIA_TARGET_SYSROOT      abs path to the target sysroot (REQUIRED for cross;
#                             empty/unset → NATIVE build, no --sysroot)
#   THEIA_TARGET_GCC_PREFIX   cross-gcc triple prefix, e.g. "aarch64-linux-gnu-"
#                             (empty → native host gcc)
#   THEIA_TARGET_CPU          aarch64 | x86_64 (CMAKE_SYSTEM_PROCESSOR)
#   THEIA_TARGET_LIB_TRIPLE   the lib multiarch dir, e.g. "aarch64-linux-gnu"
#                             (defaults to "<cpu>-linux-gnu")
#
# Usage (the foreign_cc cmake() rule / a manual build):
#   THEIA_TARGET_SYSROOT=$PWD/third_party/sysroot/jetson \
#   THEIA_TARGET_GCC_PREFIX=aarch64-linux-gnu- THEIA_TARGET_CPU=aarch64 \
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-cross.cmake ...
#
# Prereqs (cross): gcc-<triple> + g++-<triple> on the host; the target sysroot
# bootstrapped by third_party/sysroot/setup_<name>.sh.

# --- pull the target identity from the env (the registry feeds these) ---------
if(NOT THEIA_TARGET_SYSROOT AND DEFINED ENV{THEIA_TARGET_SYSROOT})
    set(THEIA_TARGET_SYSROOT "$ENV{THEIA_TARGET_SYSROOT}")
endif()
if(NOT THEIA_TARGET_GCC_PREFIX AND DEFINED ENV{THEIA_TARGET_GCC_PREFIX})
    set(THEIA_TARGET_GCC_PREFIX "$ENV{THEIA_TARGET_GCC_PREFIX}")
endif()
if(NOT THEIA_TARGET_CPU AND DEFINED ENV{THEIA_TARGET_CPU})
    set(THEIA_TARGET_CPU "$ENV{THEIA_TARGET_CPU}")
endif()
if(NOT THEIA_TARGET_LIB_TRIPLE AND DEFINED ENV{THEIA_TARGET_LIB_TRIPLE})
    set(THEIA_TARGET_LIB_TRIPLE "$ENV{THEIA_TARGET_LIB_TRIPLE}")
endif()
if(NOT THEIA_TARGET_CPU)
    set(THEIA_TARGET_CPU "aarch64")   # the common cross case
endif()
if(NOT THEIA_TARGET_LIB_TRIPLE)
    set(THEIA_TARGET_LIB_TRIPLE "${THEIA_TARGET_CPU}-linux-gnu")
endif()

# --- target identity ----------------------------------------------------------
set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR ${THEIA_TARGET_CPU})

# --- compiler (host cross-gcc by triple prefix; empty prefix = native) --------
set(CMAKE_C_COMPILER   ${THEIA_TARGET_GCC_PREFIX}gcc)
set(CMAKE_CXX_COMPILER ${THEIA_TARGET_GCC_PREFIX}g++)

# --- NATIVE build (no sysroot): stop here, host defaults are correct ----------
if(NOT THEIA_TARGET_SYSROOT OR THEIA_TARGET_SYSROOT STREQUAL "")
    return()
endif()

# --- cross: anchor everything at the sysroot ----------------------------------
if(NOT EXISTS "${THEIA_TARGET_SYSROOT}/usr/lib/${THEIA_TARGET_LIB_TRIPLE}")
    message(FATAL_ERROR
        "Target sysroot not found at ${THEIA_TARGET_SYSROOT} "
        "(expected usr/lib/${THEIA_TARGET_LIB_TRIPLE}).\n"
        "Bootstrap it with third_party/sysroot/setup_<name>.sh.")
endif()
set(CMAKE_SYSROOT "${THEIA_TARGET_SYSROOT}")

# Compiler from the host; libs/headers/programs/packages from the sysroot.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_FIND_ROOT_PATH "${THEIA_TARGET_SYSROOT}")

# pkg-config bound to the sysroot (else it hands back host .pc files silently).
set(ENV{PKG_CONFIG_DIR} "")
set(ENV{PKG_CONFIG_LIBDIR}
    "${THEIA_TARGET_SYSROOT}/usr/lib/${THEIA_TARGET_LIB_TRIPLE}/pkgconfig:${THEIA_TARGET_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${THEIA_TARGET_SYSROOT}")

# Link-time loader sees the sysroot's transitive .so deps (libgrpc++ → dozens of
# libabsl_* the user link line never names) + the lib dir on the search path.
set(_lib "${THEIA_TARGET_SYSROOT}/usr/lib/${THEIA_TARGET_LIB_TRIPLE}")
add_link_options("-Wl,-rpath-link=${_lib}" "-L${_lib}")
