"""dist_ipk.bzl — RULE 2 (DIST): per-host .ipk/.deb from a COMMITTED serialized
manifest dir. No rig.py eval, no module extension — the reproducible deploy path.

Unlike rules/rig.bzl (RULE 1 — runs `artheia serialize-manifest` against the
Python rig at module-eval time, for the DEV `bazel build @rig//:image` path),
dist_pkg reads the committed JSON `artheia serialize-manifest` already wrote:
`dist/manifest/<host>/{execution,machine}.json`. The bazel targets live IN
execution.json (`processes[].executable`); the arch lives IN machine.json
(flat `arch`). Same JSON shapes as RULE 1, but from files — so dist needs no
Python at build time.

The bazel constraint: a rule's deps are fixed at analysis time, but the targets
are named inside execution.json (read at exec time). So `binaries` is a FIXED
filegroup of ALL buildable binaries; pack_ipk.py parses execution.json and
copies only the ones this host wants (matched by the target's package path).

Cross-arch: pass `--platforms=//rules/config:rpi4` (aarch64) etc. on the
`bazel build` invocation; the binaries filegroup cross-compiles as a unit.
`theia dist` derives the platform from machine.json and runs one invocation per
host (one `bazel build //dist/manifest:<host>_pkg --platforms=…` each).

Usage (one target per host, the macro form — `theia manifest` writes this glue):
    dist_pkg(
        name = "central",                 # → //dist/manifest:central_pkg
        manifest_dir = "//dist/manifest/central",   # default //dist/manifest/<name>
    )
→ bazel build //dist/manifest:central_pkg --platforms=//rules/config:host
"""

# The full set of buildable binaries — the fixed filegroup that satisfies the
# analysis-time dep requirement. pack_ipk picks the host's subset from the JSON
# (by execution.json's process `executable` target). Keep in sync with the rig's
# buildable processes; an extra here is harmless (skipped if no host wants it).
#
# Labels are REPO-QUALIFIED to match the same framework-vs-app partition rig.bzl's
# _abs_label applies: this .bzl is a FRAMEWORK file (loaded as @pero_theia//rules:
# from a consuming workspace), so a bare `//…` would resolve against @pero_theia.
#   - framework services → @pero_theia//services/… (the framework module)
# This default is SERVICES-ONLY — the framework runtime plane. It carries NO
# user/app binaries: a consuming workspace passes
# its own `binaries=[…]` to dist_pkg, and every `theia manifest`-generated
# dist_pkg DERIVES `binaries` from the host's execution.json, so this default is
# only the services fallback. Leaking demo app labels here dragged @@//apps/…
# into every consumer's cross-build; it is intentionally absent now.
ALL_BINARIES = [
    "@pero_theia//services/com/main:com",
    "@pero_theia//services/crypto/main:crypto",
    "@pero_theia//services/diag/main:diag",
    "@pero_theia//services/fw/main:fw",
    "@pero_theia//services/idsm/main:idsm",
    "@pero_theia//services/log/main:log",
    "@pero_theia//services/nm/main:nm",
    "@pero_theia//services/osi/main:osi",
    "@pero_theia//services/per/main:per",
    "@pero_theia//services/phm/main:phm",
    "@pero_theia//services/rds/main:rds",
    "@pero_theia//services/sm/main:sm",
    "@pero_theia//services/tsync/main:tsync",
    "@pero_theia//services/ucm/main:ucm",
    "@pero_theia//services/shwa/main:shwa",
    # NOTE: //platform/gateway is NOT here. Gateway (the PSP/AUTOSAR data plane)
    # is a CONSUMING-WORKSPACE concern, not part of standalone theia.git — and it
    # drags pcap/expat-dependent libs (cmpdecoder) that aren't in the rpi4 sysroot,
    # breaking the cross-build. No zonal/test machine's execution.json references
    # it; a consuming workspace adds its own gateway binary to this list.
    # NOTE: NO app binaries here — apps are a consuming-workspace concern
    # (a consuming workspace passes its own binaries=[…]).
]

# Shared libs bundled at /opt/theia/lib/<basename> for every host (harmless if a
# host's binaries don't need them). Empty since per now STATICALLY links the etcd
# client (//third_party:etcd_cc → cmake() libetcd-cpp-api-core.a) — there is no
# runtime libetcd-cpp-api.so to bundle anymore.
DEFAULT_LIBS = []


def _dist_pkg_impl(ctx):
    fmt = ctx.attr.format
    ext = ".ipk" if fmt == "ipk" else ".deb"
    out = ctx.actions.declare_file(ctx.attr.package + ext)
    bins = ctx.files.binaries
    libs = ctx.files.libs
    args = ctx.actions.args()
    args.add("--exec", ctx.file.execution_json)
    args.add("--machine", ctx.file.machine_json)
    args.add("--out", out)
    args.add("--package", ctx.attr.package)
    args.add("--version", ctx.attr.version)
    args.add("--format", fmt)
    # DEB MODE (consuming ws pinned to /opt/theia): the framework FCs are prebuilt
    # by the runtime/services deb, so `binaries` is empty/app-only and pack_ipk
    # must tolerate the framework processes missing from the filegroup.
    if ctx.attr.deb_mode:
        args.add("--allow-prefix-binaries")
    for b in bins:
        args.add("--bin", b)
    for l in libs:
        args.add("--lib", l)

    ctx.actions.run(
        executable = ctx.executable._packer,
        arguments = [args],
        inputs = depset(
            [ctx.file.execution_json, ctx.file.machine_json] + bins + libs),
        outputs = [out],
        mnemonic = "PackPkg",
        progress_message = "Packing %s%s from execution.json" % (ctx.attr.package, ext),
    )
    return [DefaultInfo(files = depset([out]))]


_dist_pkg = rule(
    implementation = _dist_pkg_impl,
    attrs = {
        "package": attr.string(mandatory = True),
        "version": attr.string(default = "1.0.0"),
        "execution_json": attr.label(allow_single_file = [".json"], mandatory = True),
        "machine_json": attr.label(allow_single_file = [".json"], mandatory = True),
        "binaries": attr.label_list(allow_files = True, default = ALL_BINARIES),
        "libs": attr.label_list(allow_files = True, default = DEFAULT_LIBS),
        "format": attr.string(default = "deb", values = ["deb", "ipk"]),
        "deb_mode": attr.bool(default = False),
        "_packer": attr.label(
            default = "//rules:pack_ipk",
            executable = True,
            cfg = "exec",
        ),
    },
)


def dist_pkg(name, manifest_dir = None, format = "deb", **kwargs):
    """Per-host deploy bundle from dist/manifest/<name>/{execution,machine}.json.

    `name` is the host (central / compute / …). Emits a .deb (default) or .ipk
    (format="ipk") — the SAME ar archive, dpkg-installed either way. The target is
    <name>_pkg (package=<name>); manifest_dir defaults to //dist/manifest/<name>."""
    mdir = manifest_dir or "//dist/manifest/%s" % name
    _dist_pkg(
        name = "%s_pkg" % name,
        package = name,
        format = format,
        execution_json = "%s:execution.json" % mdir,
        machine_json = "%s:machine.json" % mdir,
        **kwargs
    )


def dist_ipk(name, manifest_dir = None, **kwargs):
    """Back-compat alias — the old per-host .ipk target (<name>_ipk). Prefer
    dist_pkg (emits .deb). Kept so existing `theia manifest`-generated BUILD glue
    + callers keep working."""
    mdir = manifest_dir or "//dist/manifest/%s" % name
    _dist_pkg(
        name = "%s_ipk" % name,
        package = name,
        format = "ipk",
        execution_json = "%s:execution.json" % mdir,
        machine_json = "%s:machine.json" % mdir,
        **kwargs
    )
