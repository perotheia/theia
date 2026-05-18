"""
ARM GCC 7-2017-q4 toolchain config for Cortex-R4F (TMS570LC43xx).

This is the exact GCC version shipped with TI Code Composer Studio 8.x.
Local install: /opt/gcc-arm-none-eabi-7-2017-q4/bin/arm-none-eabi-*
"""

load("@rules_cc//cc:defs.bzl", "CcToolchainConfigInfo")
load("@rules_cc//cc:cc_toolchain_config_lib.bzl",
     "action_config",
     "feature",
     "flag_group",
     "flag_set",
     "tool",
     "tool_path",
     "with_feature_set")
load("@rules_cc//cc/private/toolchain_config:cc_toolchain_config_info.bzl", "create_cc_toolchain_config_info")
load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")

_TOOLCHAIN_ROOT = "/opt/gcc-arm-none-eabi-7-2017-q4"
_TRIPLE        = "arm-none-eabi"

# Cortex-R4F flags matching TI CCS8 / TMS570LC43xx HALCoGen output
_CORTEX_R4F_FLAGS = [
    "-mcpu=cortex-r4f",
    "-mfpu=vfpv3-d16",
    "-mfloat-abi=hard",
    "-mbig-endian",            # TMS570 is big-endian
    "-mthumb",                 # Thumb-2 for code density
    "-ffunction-sections",
    "-fdata-sections",
]

_WARN_FLAGS = [
    "-Wall",
    "-Wextra",
    "-Wno-unused-parameter",
]

def _impl(ctx):
    tool_paths = [
        tool_path(name = "gcc",     path = _TOOLCHAIN_ROOT + "/bin/" + _TRIPLE + "-gcc"),
        tool_path(name = "g++",     path = _TOOLCHAIN_ROOT + "/bin/" + _TRIPLE + "-g++"),
        tool_path(name = "ar",      path = _TOOLCHAIN_ROOT + "/bin/" + _TRIPLE + "-ar"),
        tool_path(name = "cpp",     path = _TOOLCHAIN_ROOT + "/bin/" + _TRIPLE + "-cpp"),
        tool_path(name = "gcov",    path = _TOOLCHAIN_ROOT + "/bin/" + _TRIPLE + "-gcov"),
        tool_path(name = "ld",      path = _TOOLCHAIN_ROOT + "/bin/" + _TRIPLE + "-ld"),
        tool_path(name = "nm",      path = _TOOLCHAIN_ROOT + "/bin/" + _TRIPLE + "-nm"),
        tool_path(name = "objdump", path = _TOOLCHAIN_ROOT + "/bin/" + _TRIPLE + "-objdump"),
        tool_path(name = "strip",   path = _TOOLCHAIN_ROOT + "/bin/" + _TRIPLE + "-strip"),
        tool_path(name = "dwp",     path = "/usr/bin/dwp"),
    ]

    default_compile_flags = feature(
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
                flag_groups = [flag_group(flags = _CORTEX_R4F_FLAGS + _WARN_FLAGS)],
            ),
        ],
    )

    default_link_flags = feature(
        name = "default_link_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [ACTION_NAMES.cpp_link_executable],
                flag_groups = [flag_group(flags = [
                    "-mbig-endian",
                    "-mcpu=cortex-r4f",
                    "-mfpu=vfpv3-d16",
                    "-mfloat-abi=hard",
                    "-Wl,--gc-sections",
                    "-specs=nosys.specs",
                ])],
            ),
        ],
    )

    opt_feature = feature(
        name = "opt",
        flag_sets = [
            flag_set(
                actions = [ACTION_NAMES.c_compile, ACTION_NAMES.cpp_compile],
                flag_groups = [flag_group(flags = ["-O2", "-DNDEBUG"])],
            ),
        ],
    )

    dbg_feature = feature(
        name = "dbg",
        flag_sets = [
            flag_set(
                actions = [ACTION_NAMES.c_compile, ACTION_NAMES.cpp_compile],
                flag_groups = [flag_group(flags = ["-g3", "-O0"])],
            ),
        ],
    )

    return create_cc_toolchain_config_info(
        ctx = ctx,
        toolchain_identifier = "arm-gcc-7-2017q4-cortex-r4f",
        target_system_name   = "arm-none-eabi",
        target_cpu            = "cortex-r4f",
        target_libc           = "none",
        compiler              = "arm-none-eabi-gcc",
        abi_version           = "eabi",
        abi_libc_version      = "none",
        tool_paths            = tool_paths,
        cxx_builtin_include_directories = [
            _TOOLCHAIN_ROOT + "/arm-none-eabi/include",
            _TOOLCHAIN_ROOT + "/lib/gcc/arm-none-eabi/7.2.1/include",
            _TOOLCHAIN_ROOT + "/lib/gcc/arm-none-eabi/7.2.1/include-fixed",
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
    attrs = {},
    provides = [CcToolchainConfigInfo],
)
