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


    compile_flags = feature(
        name = "default_compile_flags",
        enabled = True,
        flag_sets = [flag_set(
            actions = _ALL_COMPILE,
            flag_groups = [flag_group(flags = [
                "--sysroot=" + sysroot_abs,
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
        cxx_builtin_include_directories = [
            "/usr/lib/gcc-cross/" + _TRIPLE,
            "/usr/" + _TRIPLE + "/include",
            sysroot_abs + "/usr/include",
            sysroot_abs + "/usr/include/" + _TRIPLE,
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
