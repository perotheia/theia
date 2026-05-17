"""
TI ARM CGT 18.1.1.LTS toolchain config for Cortex-R4F (TMS570LC43xx).

Compiler: armcl (TI proprietary, NOT gcc)
Install:  /opt/ti/cgt_arm_18.1.1.LTS/
Target:   TMS570LC43xx — Cortex-R4F, big-endian, VFPv3-D16, Thumb-2 (code_state=16)
"""

load("@rules_cc//cc:cc_toolchain_config_lib.bzl",
     "feature",
     "flag_group",
     "flag_set",
     "tool_path")
load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")

_CGT_ROOT = "/opt/ti/cgt_arm_18.1.1.LTS"

# armcl compile flags matching TMS570LC43xx / CCS project settings
_ARMCL_COMPILE_FLAGS = [
    "--silicon_version=7R4",    # Cortex-R4
    "--code_state=16",          # Thumb-2 instruction set
    "--float_support=VFPv3D16", # VFPv3-D16 FPU
    "--endian=big",             # TMS570 is big-endian
    "--abi=eabi",               # EABI calling convention
    "-O2",                      # optimisation level 2
    "--c99",                    # C99 language standard
    "--diag_warning=225",       # pointless-ptr-qual → warning only
]

_ARMCL_LINK_FLAGS = [
    "--silicon_version=7R4",
    "--code_state=16",
    "--float_support=VFPv3D16",
    "--endian=big",
]

def _impl(ctx):
    tool_paths = [
        # armcl doubles as C and C++ compiler
        tool_path(name = "gcc",     path = _CGT_ROOT + "/bin/armcl"),
        tool_path(name = "g++",     path = _CGT_ROOT + "/bin/armcl"),
        tool_path(name = "ar",      path = _CGT_ROOT + "/bin/armar"),
        tool_path(name = "cpp",     path = _CGT_ROOT + "/bin/armcl"),
        tool_path(name = "ld",      path = _CGT_ROOT + "/bin/armlnk"),
        tool_path(name = "nm",      path = _CGT_ROOT + "/bin/armnm"),
        tool_path(name = "objdump", path = _CGT_ROOT + "/bin/armdis"),
        tool_path(name = "strip",   path = _CGT_ROOT + "/bin/armstrip"),
        # No native gcov — point to /bin/false so Bazel doesn't hard-fail
        tool_path(name = "gcov",    path = "/bin/false"),
        tool_path(name = "dwp",     path = "/bin/false"),
    ]

    default_compile_flags = feature(
        name    = "default_compile_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.assemble,
                    ACTION_NAMES.preprocess_assemble,
                ],
                flag_groups = [flag_group(flags = _ARMCL_COMPILE_FLAGS)],
            ),
        ],
    )

    default_link_flags = feature(
        name    = "default_link_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [ACTION_NAMES.cpp_link_executable],
                flag_groups = [flag_group(flags = _ARMCL_LINK_FLAGS)],
            ),
        ],
    )

    opt_feature = feature(
        name = "opt",
        flag_sets = [
            flag_set(
                actions = [ACTION_NAMES.c_compile, ACTION_NAMES.cpp_compile],
                flag_groups = [flag_group(flags = ["-O3"])],
            ),
        ],
    )

    dbg_feature = feature(
        name = "dbg",
        flag_sets = [
            flag_set(
                actions = [ACTION_NAMES.c_compile, ACTION_NAMES.cpp_compile],
                flag_groups = [flag_group(flags = ["-g", "--opt_level=0"])],
            ),
        ],
    )

    return cc_common.create_cc_toolchain_config_info(
        ctx                  = ctx,
        toolchain_identifier = "ti-arm-cgt-18.1.1.lts-cortex-r4f",
        target_system_name   = "arm-none-eabi",
        target_cpu            = "cortex-r4f",
        target_libc           = "none",
        compiler              = "armcl-18.1.1.LTS",
        abi_version           = "eabi",
        abi_libc_version      = "none",
        tool_paths            = tool_paths,
        cxx_builtin_include_directories = [
            _CGT_ROOT + "/include",
        ],
        features = [
            default_compile_flags,
            default_link_flags,
            opt_feature,
            dbg_feature,
        ],
    )

cc_toolchain_config = rule(
    implementation = _impl,
    attrs          = {},
    provides       = [CcToolchainConfigInfo],
)
