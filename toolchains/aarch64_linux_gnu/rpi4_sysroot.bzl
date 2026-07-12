"""Repository rules capturing ABSOLUTE paths to the gitignored aarch64
sysroots (third_party/sysroot/{rpi4,orin}), so the cross cc_toolchain can pass
a real absolute --sysroot regardless of where the repo lives.

ANCHORING: the path is derived from the FRAMEWORK module's own root (resolved
via a label into this module), NOT rctx.workspace_root — a CONSUMING workspace
(bazel_dep on pero_theia) has its own workspace root with no sysroot under it,
which silently produced a dangling --sysroot for ws cross builds. Env overrides
(THEIA_RPI4_SYSROOT / THEIA_ORIN_SYSROOT) still win.
"""

def _module_root(rctx, anchor):
    # anchor = a label to //:MODULE.bazel IN THIS MODULE; its resolved path's
    # parent is the module root wherever the module is (main repo or bzlmod
    # dep). REALPATH, not the external-repo symlink path: gcc canonicalizes
    # sysroot'd includes in its .d output, and bazel's strict-header check
    # compares those REAL paths against the toolchain's builtin dirs — a
    # symlinked prefix here would never match (seen from a consuming ws:
    # external/pero_theia+/... vs /home/.../repo/theia/...).
    return str(rctx.path(anchor).realpath.dirname)

def _rpi4_sysroot_impl(rctx):
    override = rctx.os.environ.get("THEIA_RPI4_SYSROOT", "")
    if override:
        path = override
    else:
        path = _module_root(rctx, rctx.attr._anchor) + "/third_party/sysroot/rpi4"
    rctx.file("BUILD.bazel", 'exports_files(["path.bzl"])\n')
    rctx.file("path.bzl", 'RPI4_SYSROOT = "%s"\n' % path)

rpi4_sysroot = repository_rule(
    implementation = _rpi4_sysroot_impl,
    attrs = {"_anchor": attr.label(default = Label("//:MODULE.bazel"))},
    environ = ["THEIA_RPI4_SYSROOT"],
    local = True,
)

def _orin_sysroot_impl(rctx):
    override = rctx.os.environ.get("THEIA_ORIN_SYSROOT", "")
    if override:
        path = override
    else:
        path = _module_root(rctx, rctx.attr._anchor) + "/third_party/sysroot/orin"
    rctx.file("BUILD.bazel", 'exports_files(["path.bzl"])\n')
    rctx.file("path.bzl", 'ORIN_SYSROOT = "%s"\n' % path)

# Same shape for the JAMMY (Jetson Orin) sysroot — setup_orin.sh bootstraps it.
orin_sysroot = repository_rule(
    implementation = _orin_sysroot_impl,
    attrs = {"_anchor": attr.label(default = Label("//:MODULE.bazel"))},
    environ = ["THEIA_ORIN_SYSROOT"],
    local = True,
)
