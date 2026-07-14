"""aarch64-linux-gnu toolchain config — cross-compile to Raspberry Pi 4 (aarch64,
Debian bookworm). Uses the host's `gcc-aarch64-linux-gnu` (apt) + the bookworm
aarch64 sysroot at third_party/sysroot/rpi4/. Mirrors cmake/toolchain-rpi4.cmake
for the Bazel cc path, so `--platforms=//rules/config:rpi4` builds the supervisor.
"""

load("@rules_cc//cc:defs.bzl", "CcToolchainConfigInfo")
load("@rules_cc//cc:cc_toolchain_config_lib.bzl",
     "feature", "flag_group", "flag_set", "tool_path")
load("@rules_cc//cc/private/toolchain_config:cc_toolchain_config_info.bzl",
     "create_cc_toolchain_config_info")
load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")

_TRIPLE = "aarch64-linux-gnu"
# The bookworm aarch64 rootfs (bootstrapped by third_party/sysroot/setup_rpi4.sh).
_SYSROOT = "external/_main/third_party/sysroot/rpi4"

_ALL_COMPILE = [
    ACTION_NAMES.c_compile,
    ACTION_NAMES.cpp_compile,
    ACTION_NAMES.linkstamp_compile,
    ACTION_NAMES.assemble,
    ACTION_NAMES.preprocess_assemble,
    ACTION_NAMES.cpp_header_parsing,
    ACTION_NAMES.cpp_module_compile,
    ACTION_NAMES.cpp_module_codegen,
]
_ALL_LINK = [
    ACTION_NAMES.cpp_link_executable,
    ACTION_NAMES.cpp_link_dynamic_library,
    ACTION_NAMES.cpp_link_nodeps_dynamic_library,
]

def _impl(ctx):
    # Absolute path to the gitignored bookworm aarch64 sysroot (host data outside
    # the sandbox; rpi4 actions run un-sandboxed via .bazelrc `build:rpi4`).
    sysroot_abs = ctx.attr.sysroot

    tool_paths = [
        tool_path(name = "gcc",     path = "/usr/bin/%s-gcc" % _TRIPLE),
        tool_path(name = "g++",     path = "/usr/bin/%s-g++" % _TRIPLE),
        tool_path(name = "ar",      path = "/usr/bin/%s-ar" % _TRIPLE),
        tool_path(name = "cpp",     path = "/usr/bin/%s-cpp" % _TRIPLE),
        tool_path(name = "gcov",    path = "/usr/bin/%s-gcov" % _TRIPLE),
        tool_path(name = "ld",      path = "/usr/bin/%s-ld" % _TRIPLE),
        tool_path(name = "nm",      path = "/usr/bin/%s-nm" % _TRIPLE),
        tool_path(name = "objcopy", path = "/usr/bin/%s-objcopy" % _TRIPLE),
        tool_path(name = "objdump", path = "/usr/bin/%s-objdump" % _TRIPLE),
        tool_path(name = "strip",   path = "/usr/bin/%s-strip" % _TRIPLE),
    ]


    # The cross-gcc's own builtin include dirs (host-provided by gcc-cross).
    # <gcc_cross>/11 is the compiler-version root; libstdc++ and the multiarch
    # libc headers hang off it. See the include-search-order note below.
    _gcc_cross = "/usr/lib/gcc-cross/" + _TRIPLE + "/11"

    compile_flags = feature(
        name = "default_compile_flags",
        enabled = True,
        flag_sets = [flag_set(
            actions = _ALL_COMPILE,
            flag_groups = [flag_group(flags = [
                "--sysroot=" + sysroot_abs,
                # GLIBC-VERSION LEAK FIX (orin/jammy cross on a noble build host):
                # `--sysroot` ADDS the sysroot's headers but does NOT remove the
                # cross-gcc's baked-in multiarch dir /usr/<triple>/include, which
                # on a noble host ships glibc 2.39 headers. Those headers redirect
                # strtol()→__isoc23_strtol() (a GLIBC_2.38 symbol) — so binaries
                # cross-built here reference GLIBC_2.38 and fail to run on the
                # jammy target (glibc 2.35). The dir sorts BEFORE the sysroot's
                # /usr/include, so it wins. `-nostdinc` drops ALL default include
                # dirs; we then re-add exactly the ones we want, in order, EXCEPT
                # the noble multiarch dir — so <stdlib.h> resolves from the jammy
                # sysroot (2.35, no __isoc23_ redirect). The libstdc++ headers
                # (arch-neutral template code, gcc-11 → GLIBCXX 3.4.30-max) and
                # gcc's own builtin C headers stay. Order mirrors g++'s native
                # search list with the one host-glibc entry removed.
                "-nostdinc",
                "-isystem", _gcc_cross + "/../../../../" + _TRIPLE + "/include/c++/11",
                "-isystem", _gcc_cross + "/../../../../" + _TRIPLE + "/include/c++/11/" + _TRIPLE,
                "-isystem", _gcc_cross + "/../../../../" + _TRIPLE + "/include/c++/11/backward",
                "-isystem", _gcc_cross + "/include",
                "-isystem", _gcc_cross + "/include-fixed",
                # (noble /usr/<triple>/include DELIBERATELY OMITTED — the leak.)
                # A cross gcc does NOT search <sysroot>/usr/local/include (the
                # native-only LOCAL_INCLUDE_DIR) — but a sysroot carrying a
                # from-source closure (orin: grpc/protobuf 3.21 in /usr/local,
                # setup_orin.sh) serves its headers from there. Harmless when
                # the dir is empty (rpi4/bookworm: everything is in /usr/include).
                "-isystem", sysroot_abs + "/usr/local/include",
                "-isystem", sysroot_abs + "/usr/include/" + _TRIPLE,
                "-isystem", sysroot_abs + "/usr/include",
                "-no-canonical-prefixes",
                "-fPIC",
                "-Wall",
            ])],
        )],
    )
    link_flags = feature(
        name = "default_link_flags",
        enabled = True,
        flag_sets = [flag_set(
            actions = _ALL_LINK,
            flag_groups = [flag_group(flags = [
                "--sysroot=" + sysroot_abs,
                "-no-canonical-prefixes",
                "-lstdc++",
                "-lpthread",
            ])],
        )],
    )

    return create_cc_toolchain_config_info(
        ctx = ctx,
        toolchain_identifier = "aarch64-linux-gnu",
        host_system_name     = "x86_64-unknown-linux-gnu",
        target_system_name   = _TRIPLE,
        target_cpu           = "aarch64",
        target_libc          = "glibc",
        compiler             = "gcc",
        abi_version          = "aarch64",
        abi_libc_version     = "glibc",
        tool_paths           = tool_paths,
        features             = [compile_flags, link_flags],
        # The cross-gcc's own headers + the sysroot's system headers.
        # sysroot_abs = <ws>/third_party/sysroot/rpi4, so <sysroot_abs>/../..
        # is <ws>/third_party — the etcd-cpp-apiv3 submodule lives there. Its
        # headers are consumed system-style (#include <etcd/SyncClient.hpp>) and
        # the foreign_cc cmake() build exposes them by ABSOLUTE source path; the
        # host toolchain tolerates that, but this cross toolchain's strict header
        # scan rejects it. Declaring the third_party root builtin (these ARE
        # external system-style headers) is what builtin-include-dirs is for.
        cxx_builtin_include_directories = [
            "/usr/lib/gcc-cross/" + _TRIPLE,
            # Noble host multiarch dir — NO LONGER on the compile search path
            # (see the -nostdinc GLIBC-leak fix above) but kept declared so any
            # stray reference is still accepted by the strict header scan rather
            # than erroring; it is simply never reached during compiles now.
            "/usr/" + _TRIPLE + "/include",
            # Consuming-workspace .bazelrc adds -I/usr/include/nanopb for the
            # host pb.h layout; declare it builtin so the cross analysis accepts
            # the flag (nanopb's pb.h is arch-neutral C — harmless either way).
            "/usr/include/nanopb",
            sysroot_abs + "/usr/local/include",
            sysroot_abs + "/usr/include",
            sysroot_abs + "/usr/include/" + _TRIPLE,
            sysroot_abs + "/../../etcd-cpp-apiv3",
        ],
        builtin_sysroot = sysroot_abs,
    )

cc_toolchain_config = rule(
    implementation = _impl,
    attrs = {
        # Absolute path to the bookworm aarch64 sysroot. Passed from the BUILD
        # so it can be the repo-absolute path (the sysroot is gitignored data).
        "sysroot": attr.string(mandatory = True),
    },
    provides = [CcToolchainConfigInfo],
)
