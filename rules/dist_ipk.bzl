"""dist_ipk.bzl — build a per-host .ipk from JSON manifests (the deploy half of
the manifest-json pivot).

Unlike rules/rig.bzl (which runs `artheia rig-deps` against the Python rig at
module-eval time, for the DEV `bazel build @rig//:image` path), dist_ipk reads
the committed JSON manifests — `dist/manifest/<host>/{application,machine}.json`.
No rig.py, no module extension. The bazel_targets live IN application.json; the
arch lives IN machine.json.

The bazel constraint: a rule's deps are fixed at analysis time, but the targets
are named inside application.json (read at exec time). So `binaries` is a FIXED
filegroup of ALL buildable binaries; pack_ipk.py parses application.json and
copies only the ones this host wants (basename → component name).

Cross-arch: pass `--platforms=//rules/config:rpi4` (aarch64) etc. on the
`bazel build` invocation; the binaries filegroup cross-compiles as a unit.
`theia dist` derives the platform from machine.json and runs one invocation per
host.

Usage (one target per host, the (i) macro form):
    dist_ipk(
        name = "central",
        application_json = "//dist/manifest/central:application.json",
        machine_json     = "//dist/manifest/central:machine.json",
    )
→ bazel build //deploy:central_ipk --platforms=//rules/config:host
"""

# The full set of buildable binaries — the fixed filegroup that satisfies the
# analysis-time dep requirement. pack_ipk picks the host's subset from the JSON.
# Keep in sync with the rig's buildable components; an extra here is harmless
# (skipped if no host wants it).
ALL_BINARIES = [
    "//platform/supervisor/main:supervisor",
    "//services/log/main:log",
    "//services/per/main:per",
    "//services/sm/main:sm",
    "//services/ucm/main:ucm",
    "//services/shwa/main:shwa",
    "//demo/Demo3WayP1/main:demo",
    "//demo/Demo3WayP2/main:demo",
    "//demo/Demo3WayP3/main:demo",
]


def _dist_ipk_impl(ctx):
    out_ipk = ctx.actions.declare_file(ctx.attr.package + ".ipk")
    bins = ctx.files.binaries
    args = ctx.actions.args()
    args.add("--app", ctx.file.application_json)
    args.add("--machine", ctx.file.machine_json)
    args.add("--out", out_ipk)
    args.add("--package", ctx.attr.package)
    args.add("--version", ctx.attr.version)
    for b in bins:
        args.add("--bin", b)

    ctx.actions.run(
        executable = ctx.executable._packer,
        arguments = [args],
        inputs = depset([ctx.file.application_json, ctx.file.machine_json] + bins),
        outputs = [out_ipk],
        mnemonic = "PackIpk",
        progress_message = "Packing %s.ipk from application.json" % ctx.attr.package,
    )
    return [DefaultInfo(files = depset([out_ipk]))]


_dist_ipk = rule(
    implementation = _dist_ipk_impl,
    attrs = {
        "package": attr.string(mandatory = True),
        "version": attr.string(default = "1.0.0"),
        "application_json": attr.label(allow_single_file = [".json"], mandatory = True),
        "machine_json": attr.label(allow_single_file = [".json"], mandatory = True),
        "binaries": attr.label_list(allow_files = True, default = ALL_BINARIES),
        "_packer": attr.label(
            default = "//rules:pack_ipk",
            executable = True,
            cfg = "exec",
        ),
    },
)


def dist_ipk(name, manifest_dir = None, **kwargs):
    """Per-host .ipk from dist/manifest/<name>/{application,machine}.json.

    `name` is the host (central / compute / …). manifest_dir defaults to
    //dist/manifest/<name>. The .ipk target is <name>_ipk (package=<name>)."""
    mdir = manifest_dir or "//dist/manifest/%s" % name
    _dist_ipk(
        name = "%s_ipk" % name,
        package = name,
        application_json = "%s:application.json" % mdir,
        machine_json = "%s:machine.json" % mdir,
        **kwargs
    )
