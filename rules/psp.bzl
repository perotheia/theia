"""psp.bzl — Starlark rules replacing generate.sh for PSP code generation.

Public rules:
  psp_generate  — runs `artheia gen-platform-protos`, outputs two TreeArtifacts
  psp_library   — same, then compiles generated .c files into a static .a

DBC files are tracked via attr.label_keyed_string_dict so Bazel invalidates
the action when any DBC or the FIBEX file changes.

`artheia` must be on $PATH for the Bazel action — install the workspace venv
with `pip install -e artheia/` and invoke bazel as
`PATH="$PWD/.venv/bin:$PATH" bazel build ...` (per workspace convention).
"""

def _resolve_dbc_args(ctx):
    """Return (inputs_list, args_list) for --dbc flags."""
    inputs = []
    dbc_args = []
    for f, bus_name in ctx.attr.dbc_specs.items():
        files = f.files.to_list()
        if len(files) != 1:
            fail("dbc_specs entry for bus '{}' must resolve to exactly one file".format(bus_name))
        inputs.append(files[0])
        dbc_args.append(files[0].path + ":" + bus_name)
    return inputs, dbc_args

def _build_generate_cmd(ctx, fibex, dbc_files, dbc_flag_vals, src_dir, proto_dir):
    """Return (cmd_string, all_inputs) for the generate shell action."""
    dbc_flags = " ".join(["--dbc '" + dv + "'" for dv in dbc_flag_vals])
    cmd = ("artheia gen-platform-protos" +
           " --fibex '" + fibex.path + "'" +
           " --namespace-fr '" + ctx.attr.namespace_fr + "'" +
           " --out-src '" + src_dir.path + "'" +
           " --out-proto '" + proto_dir.path + "'" +
           " --all-signals " + dbc_flags)
    all_inputs = depset([fibex] + dbc_files)
    return cmd, all_inputs

def _psp_generate_impl(ctx):
    src_dir   = ctx.actions.declare_directory(ctx.attr.name + "_src")
    proto_dir = ctx.actions.declare_directory(ctx.attr.name + "_proto")

    dbc_files, dbc_flag_vals = _resolve_dbc_args(ctx)
    cmd, all_inputs = _build_generate_cmd(
        ctx, ctx.file.fibex, dbc_files, dbc_flag_vals, src_dir, proto_dir,
    )

    ctx.actions.run_shell(
        command      = cmd,
        inputs       = all_inputs,
        outputs      = [src_dir, proto_dir],
        mnemonic     = "PspGenerate",
        progress_message = "PspGenerate %s" % ctx.label,
        execution_requirements = {"no-sandbox": "1"},
        use_default_shell_env = True,
    )

    return [
        DefaultInfo(files = depset([src_dir, proto_dir])),
        OutputGroupInfo(src = depset([src_dir]), proto = depset([proto_dir])),
    ]

_PSP_COMMON_ATTRS = {
    "fibex": attr.label(
        allow_single_file = [".xml"],
        mandatory         = True,
        doc = "FIBEX XML describing the FlexRay cluster.",
    ),
    "dbc_specs": attr.label_keyed_string_dict(
        allow_files = [".dbc"],
        mandatory   = True,
        doc = "Maps each DBC file label to its bus name string, e.g. {'//mlbevo_gen2_cmp_psp/config/dbc/KCAN.dbc': 'kcan'}.",
    ),
    "namespace_fr": attr.string(
        mandatory = True,
        doc = "FlexRay namespace prefix (e.g. 'mlbevo_gen2').",
    ),
}

psp_generate = rule(
    implementation = _psp_generate_impl,
    attrs          = _PSP_COMMON_ATTRS,
)

# ── psp_library ───────────────────────────────────────────────────────────────

def _psp_library_impl(ctx):
    src_dir   = ctx.actions.declare_directory(ctx.attr.name + "_src")
    proto_dir = ctx.actions.declare_directory(ctx.attr.name + "_proto")

    dbc_files, dbc_flag_vals = _resolve_dbc_args(ctx)
    cmd, gen_inputs = _build_generate_cmd(
        ctx, ctx.file.fibex, dbc_files, dbc_flag_vals, src_dir, proto_dir,
    )

    ctx.actions.run_shell(
        command      = cmd,
        inputs       = gen_inputs,
        outputs      = [src_dir, proto_dir],
        mnemonic     = "PspGenerate",
        progress_message = "PspGenerate %s" % ctx.label,
        execution_requirements = {"no-sandbox": "1"},
        use_default_shell_env = True,
    )

    # Compile all generated .c files into a static archive.
    lib_out = ctx.actions.declare_file(ctx.attr.name + ".a")

    includes = " ".join(["-I" + d for d in ctx.attr.cc_include_dirs])

    compile_script = """
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/usr/libexec
set -e
GCC=/usr/bin/gcc
SRC="{src}"
OUT="{lib}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
FAILED=0
for f in $(find "$SRC" -name '*.c'); do
    OBJ="$TMP/$(echo "$f" | tr '/' '_').o"
    $GCC -c -std=c11 -fPIC -O2 {inc} -I"$SRC" "$f" -o "$OBJ" 2>&1 || FAILED=1
done
if [ "$FAILED" -ne 0 ]; then echo "PSP compilation errors above"; exit 1; fi
OBJS=$(find "$TMP" -name '*.o' | tr '\\n' ' ')
if [ -z "$OBJS" ]; then
    ar rcs "$OUT"
else
    ar rcs "$OUT" $OBJS
fi
""".format(src=src_dir.path, lib=lib_out.path, inc=includes)

    ctx.actions.run_shell(
        command      = compile_script,
        inputs       = depset([src_dir]),
        outputs      = [lib_out],
        mnemonic     = "PspCompile",
        progress_message = "PspCompile %s" % lib_out.basename,
        execution_requirements = {"no-sandbox": "1"},
    )

    # DefaultInfo carries only the .a so cc_binary/cc_library can use it in srcs.
    # Generated src + proto dirs are available via output groups if needed.
    return [
        DefaultInfo(files = depset([lib_out])),
        OutputGroupInfo(
            generated_src   = depset([src_dir]),
            generated_proto = depset([proto_dir]),
        ),
    ]

psp_library = rule(
    implementation = _psp_library_impl,
    attrs = dict(
        _PSP_COMMON_ATTRS,
        cc_include_dirs = attr.string_list(
            default = [],
            doc = "Extra -I dirs when compiling generated .c files.",
        ),
    ),
)
