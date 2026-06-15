"""A repository rule that captures the ABSOLUTE path to the gitignored bookworm
aarch64 sysroot (third_party/sysroot/rpi4), so the cross cc_toolchain can pass a
real absolute --sysroot regardless of where the repo lives (dev host or /opt in
CI). The sysroot is host data outside bazel's sandbox, so a relative path won't
do. Override with the THEIA_RPI4_SYSROOT env var.
"""

def _rpi4_sysroot_impl(rctx):
    override = rctx.os.environ.get("THEIA_RPI4_SYSROOT", "")
    if override:
        path = override
    else:
        # rctx.workspace_root is the absolute path to the main repo root.
        path = str(rctx.workspace_root) + "/third_party/sysroot/rpi4"
    rctx.file("BUILD.bazel", 'exports_files(["path.bzl"])\n')
    rctx.file("path.bzl", 'RPI4_SYSROOT = "%s"\n' % path)

rpi4_sysroot = repository_rule(
    implementation = _rpi4_sysroot_impl,
    environ = ["THEIA_RPI4_SYSROOT"],
    local = True,
)
