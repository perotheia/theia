"""TI ARM CGT 18.1.1.LTS toolchain config for TMS570LC43xx (Cortex-R4F, be32).

Compiler: armcl  (TI proprietary, not GCC)
Linker:   armcl -z
Archiver: armar
Install:  /opt/ti/cgt_arm_18.1.1.LTS/

CCS 8.0.0 project settings (from .ccsproject):
  silicon_version  = 7R4
  code_state       = 16   (Thumb-2)
  float_support    = VFPv3D16
  endian           = big (be32)
  abi              = eabi
  rts              = rtsv7R4_T_be_v3D16_eabi.lib
"""

load("@rules_cc//cc:cc_toolchain_config_lib.bzl",
     "feature", "flag_group", "flag_set", "tool_path")
load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")

_ARMCL  = "/opt/ti/cgt_arm_18.1.1.LTS/bin/armcl"
_ARMAR  = "/opt/ti/cgt_arm_18.1.1.LTS/bin/armar"
_ARMDIS = "/opt/ti/cgt_arm_18.1.1.LTS/bin/armdis"
_ARMNM  = "/opt/ti/cgt_arm_18.1.1.LTS/bin/armnm"

_CORE_FLAGS = [
    "--silicon_version=7R4",
    "--code_state=16",
    "--endian=big",
    "--float_support=VFPv3D16",
    "--abi=eabi",
    "--c99",
    "--diag_suppress=10063",  # typedef redeclaration (lwIP arch.h)
    "--diag_suppress=230",    # expression has no effect (mpack.h)
]

_LINK_FLAGS = [
    "-z",
    "--silicon_version=7R4",
    "--code_state=16",
    "--endian=big",
    "--float_support=VFPv3D16",
    "--abi=eabi",
    "--be32",
    "--unused_section_elimination=on",
    "-l", "/opt/ti/cgt_arm_18.1.1.LTS/lib/rtsv7R4_T_be_v3D16_eabi.lib",
]

def _impl(ctx):
    tool_paths = [
        tool_path(name = "gcc",     path = _ARMCL),
        tool_path(name = "g++",     path = _ARMCL),
        tool_path(name = "cpp",     path = _ARMCL),
        tool_path(name = "ar",      path = _ARMAR),
        tool_path(name = "ld",      path = _ARMCL),
        tool_path(name = "nm",      path = _ARMNM),
        tool_path(name = "objdump", path = _ARMDIS),
        tool_path(name = "strip",   path = _ARMCL),
        tool_path(name = "gcov",    path = "/usr/bin/true"),
        tool_path(name = "dwp",     path = "/usr/bin/true"),
    ]

    compile_flags = feature(
        name = "default_compile_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.assemble,
                    ACTION_NAMES.preprocess_assemble,
                ],
                flag_groups = [flag_group(flags = _CORE_FLAGS + ["-O2"])],
            ),
        ],
    )

    link_flags = feature(
        name = "default_link_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [ACTION_NAMES.cpp_link_executable],
                flag_groups = [flag_group(flags = _LINK_FLAGS)],
            ),
            flag_set(
                actions = [ACTION_NAMES.cpp_link_static_library],
                flag_groups = [flag_group(flags = ["rqs"])],
            ),
        ],
    )

    dbg = feature(
        name = "dbg",
        flag_sets = [flag_set(
            actions = [ACTION_NAMES.c_compile, ACTION_NAMES.cpp_compile],
            flag_groups = [flag_group(flags = ["-O0", "-g"])],
        )],
    )

    return cc_common.create_cc_toolchain_config_info(
        ctx                          = ctx,
        toolchain_identifier         = "ti-arm-cgt-18-tms570lc43xx",
        target_system_name           = "arm-none-eabi",
        target_cpu                   = "cortex-r4f",
        target_libc                  = "none",
        compiler                     = "armcl",
        abi_version                  = "eabi",
        abi_libc_version             = "none",
        tool_paths                   = tool_paths,
        cxx_builtin_include_directories = [
            "/opt/ti/cgt_arm_18.1.1.LTS/include",
        ],
        features = [compile_flags, link_flags, dbg],
    )

cc_toolchain_config = rule(
    implementation = _impl,
    attrs          = {},
    provides       = [CcToolchainConfigInfo],
)
