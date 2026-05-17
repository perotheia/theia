"""opkg.bzl — Bazel rule for building .ipk packages (opkg/OpenWrt format).

An .ipk is an ar(1) archive: debian-binary + control.tar.gz + data.tar.gz.
"""

def _render_control(attrs):
    lines = [
        "Package: " + attrs.package,
        "Version: " + attrs.version,
        "Architecture: " + attrs.arch,
        "Section: " + attrs.section,
        "Priority: " + attrs.priority,
        "Maintainer: " + attrs.maintainer,
    ]
    if attrs.depends:
        lines.append("Depends: " + attrs.depends)
    if attrs.provides:
        lines.append("Provides: " + attrs.provides)
    lines.append("Description: " + attrs.description)
    return "\n".join(lines) + "\n"

def _pkg_opkg_impl(ctx):
    src_files = []
    dest_paths = []
    for src_label, dest_path in ctx.attr.files.items():
        fl = src_label.files.to_list()
        if len(fl) != 1:
            fail("files entry must resolve to exactly 1 file, got %d for %s" % (len(fl), dest_path))
        src_files.append(fl[0])
        dest_paths.append(dest_path)

    ipk_name = ctx.attr.package + "_" + ctx.attr.version + "_" + ctx.attr.arch + ".ipk"
    out_ipk = ctx.actions.declare_file(ipk_name)

    control_content = _render_control(ctx.attr)

    # Build copy commands
    copy_lines = []
    for i, dest in enumerate(dest_paths):
        mode = "755" if (dest.startswith("/usr/bin") or dest.startswith("/usr/sbin")) else "644"
        copy_lines.append(
            "install -D -m " + mode + " " +
            "'\"${SRCS[" + str(i) + "]}\"'" + " \"$STAGE" + dest + "\""
        )

    # Simple copy using positional args — build via numbered env vars
    copy_cmds = []
    for i in range(len(src_files)):
        dest = dest_paths[i]
        mode = "755" if (dest.startswith("/usr/bin") or dest.startswith("/usr/sbin")) else "644"
        copy_cmds.append(
            "install -D -m " + mode + " \"$SRC" + str(i) + "\" \"$STAGE" + dest + "\""
        )

    src_vars = []
    for i, f in enumerate(src_files):
        src_vars.append("SRC" + str(i) + "='" + f.path + "'")

    postinst_block = ""
    if ctx.attr.postinst:
        postinst_block = (
            "cat > \"$CTRL/postinst\" << '__POSTINST_EOF__'\n" +
            ctx.attr.postinst +
            "\n__POSTINST_EOF__\n" +
            "chmod 755 \"$CTRL/postinst\"\n"
        )

    prerm_block = ""
    if ctx.attr.prerm:
        prerm_block = (
            "cat > \"$CTRL/prerm\" << '__PRERM_EOF__'\n" +
            ctx.attr.prerm +
            "\n__PRERM_EOF__\n" +
            "chmod 755 \"$CTRL/prerm\"\n"
        )

    script = (
        "export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\n" +
        "set -e\n" +
        "\n".join(src_vars) + "\n" +
        "TMP=$(mktemp -d)\n" +
        "trap 'rm -rf \"$TMP\"' EXIT\n" +
        "STAGE=\"$TMP/data\"\n" +
        "CTRL=\"$TMP/ctrl\"\n" +
        "mkdir -p \"$STAGE\" \"$CTRL\"\n" +
        "\n".join(copy_cmds) + "\n" +
        "cat > \"$CTRL/control\" << '__CONTROL_EOF__'\n" +
        control_content +
        "__CONTROL_EOF__\n" +
        postinst_block +
        prerm_block +
        "( cd \"$STAGE\" && tar czf \"$TMP/data.tar.gz\" . )\n" +
        "( cd \"$CTRL\"  && tar czf \"$TMP/control.tar.gz\" . )\n" +
        "printf '2.0\\n' > \"$TMP/debian-binary\"\n" +
        "ar cr '" + out_ipk.path + "' \"$TMP/debian-binary\" \"$TMP/control.tar.gz\" \"$TMP/data.tar.gz\"\n" +
        "echo 'Built: " + ipk_name + "'\n"
    )

    ctx.actions.run_shell(
        command      = script,
        inputs       = depset(src_files),
        outputs      = [out_ipk],
        mnemonic     = "PkgOpkg",
        progress_message = "PkgOpkg " + ipk_name,
        execution_requirements = {"no-sandbox": "1"},
    )

    return [DefaultInfo(files = depset([out_ipk]))]

pkg_opkg = rule(
    implementation = _pkg_opkg_impl,
    attrs = {
        "package":     attr.string(mandatory = True),
        "version":     attr.string(default = "1.0.0"),
        "arch":        attr.string(default = "x86_64"),
        "section":     attr.string(default = "misc"),
        "priority":    attr.string(default = "optional"),
        "maintainer":  attr.string(default = "PERO CMP <pero-cmp@example.com>"),
        "description": attr.string(mandatory = True),
        "depends":     attr.string(default = ""),
        "provides":    attr.string(default = ""),
        "files": attr.label_keyed_string_dict(
            allow_files = True,
            mandatory   = True,
        ),
        "postinst": attr.string(default = ""),
        "prerm":    attr.string(default = ""),
    },
)
