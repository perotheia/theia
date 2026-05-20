"""nanopb.bzl — run nanopb's protoc plugin over .proto files and expose
the resulting .pb.h / .pb.c through a cc_library.

The nanopb generator lives at /home/axadmin/.local/bin/nanopb_generator
(pipx-installed). Apps and the gateway service consume the produced
typed C structs (e.g. shared_ACC_07) instead of decoding the protobuf
wire bytes by hand.

Usage:
    load("//rules:nanopb.bzl", "nanopb_generate")

    nanopb_generate(name = "psp_nanopb", srcs = [":proto_files"])

The output is a TreeArtifact. Downstream cc_library can put it in
`hdrs` and add the TreeArtifact's directory to `includes`.
"""

def _nanopb_generate_impl(ctx):
    out_dir = ctx.actions.declare_directory(ctx.attr.name + "_pb")

    # Gather all .proto files from srcs (each src may be a filegroup or
    # a rule that ships a directory of protos via DefaultInfo).
    all_files = []
    for src in ctx.attr.srcs:
        all_files.extend(src[DefaultInfo].files.to_list())

    # The shell script: for every input (either a .proto or a directory
    # of .proto files), run nanopb_generator with the proto-root set so
    # the `package shared;` lines map to relative output paths.
    cmd_lines = [
        "set -e",
        "OUT='" + out_dir.path + "'",
        "mkdir -p \"$OUT\"",
        "NPB=/home/axadmin/.local/bin/nanopb_generator",
    ]
    for f in all_files:
        # If `f` is a TreeArtifact (directory), iterate its contents at
        # action time via `find`. Otherwise pass directly. We can't tell
        # is_directory from Starlark, so use a defensive find-and-glob.
        cmd_lines.append(
            "for p in $(find '" + f.path + "' -name '*.proto' 2>/dev/null || echo '" + f.path + "'); do " +
            "ROOT=\"$(dirname \"$p\")\"; " +
            "while [ \"$(basename \"$ROOT\")\" != \"shared\" ] && " +
            "      [ \"$(basename \"$ROOT\")\" != \"can\" ] && " +
            "      [ \"$(basename \"$ROOT\")\" != \"flexray\" ] && " +
            "      [ \"$ROOT\" != \"/\" ]; do " +
            "  ROOT=\"$(dirname \"$ROOT\")\"; " +
            "done; " +
            "ROOT=\"$(dirname \"$ROOT\")\"; " +
            "\"$NPB\" -I \"$ROOT\" -D \"$OUT\" \"$p\"; " +
            "done",
        )

    ctx.actions.run_shell(
        command = "\n".join(cmd_lines),
        inputs = depset(all_files),
        outputs = [out_dir],
        mnemonic = "NanopbGenerate",
        progress_message = "NanopbGenerate %s" % ctx.label,
        execution_requirements = {"no-sandbox": "1"},
        use_default_shell_env = True,
    )

    return [
        DefaultInfo(files = depset([out_dir])),
        OutputGroupInfo(out = depset([out_dir])),
    ]

nanopb_generate = rule(
    implementation = _nanopb_generate_impl,
    attrs = {
        "srcs": attr.label_list(
            mandatory = True,
            doc = "Targets producing .proto files (e.g. filegroup over PspGenerate's :proto output group).",
        ),
    },
)
