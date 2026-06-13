"""deb.bzl — Bazel rule for building Debian .deb packages.

A .deb is an ar(1) archive: debian-binary + control.tar.gz + data.tar.gz — the
SAME on-disk format as the .ipk that rules/opkg.bzl builds, so this rule is a
near-clone of pkg_opkg with three .deb-isms:

  * output name  <package>_<version>_<arch>.deb  (arch = amd64/arm64/all, the
    Debian names, not the opkg x86_64/aarch64);
  * control file carries Installed-Size (dpkg shows it; apt sums it);
  * `data` accepts filegroups/trees (a runtime ships whole include/ + src/
    subtrees), staged preserving their workspace-relative path under `prefix`.

`files` keeps the pkg_opkg single-file → explicit-dest mapping for binaries; use
`data` + `prefix` for source/header trees. Both land in the same data.tar.gz.

This is Theia's OWN packaging (dist/debian/); the embedded/opkg path stays on
pkg_opkg (dist/ipkg/). The two share rules/opkg.bzl's _render_control shape.
"""

# Bazel CPU constraint value → Debian architecture name.
_DEB_ARCH = {
    "x86_64": "amd64",
    "k8": "amd64",
    "aarch64": "arm64",
    "arm64": "arm64",
    "all": "all",
}

def _render_control(attrs, arch, installed_kb):
    lines = [
        "Package: " + attrs.package,
        "Version: " + attrs.version,
        "Architecture: " + arch,
        "Section: " + attrs.section,
        "Priority: " + attrs.priority,
        "Maintainer: " + attrs.maintainer,
        "Installed-Size: " + str(installed_kb),
    ]
    if attrs.depends:
        lines.append("Depends: " + attrs.depends)
    if attrs.provides:
        lines.append("Provides: " + attrs.provides)
    lines.append("Description: " + attrs.description)
    return "\n".join(lines) + "\n"

def _exec_mode(dest):
    # Files under any bin/ or sbin/ dir are executables (0755); else data (0644).
    # Matches opkg.bzl: /opt/theia/bin/<name> is the FC binary layout.
    return "755" if ("/bin/" in dest or "/sbin/" in dest) else "644"

def _pkg_deb_impl(ctx):
    arch = _DEB_ARCH.get(ctx.attr.arch, ctx.attr.arch)

    # (1) explicit single-file → dest entries (binaries, configs).
    src_files = []
    copy_cmds = []
    for src_label, dest_path in ctx.attr.files.items():
        fl = src_label.files.to_list()
        if len(fl) != 1:
            fail("files entry must resolve to exactly 1 file, got %d for %s" %
                 (len(fl), dest_path))
        idx = len(src_files)
        src_files.append(fl[0])
        copy_cmds.append(
            "install -D -m " + _exec_mode(dest_path) +
            " \"$SRC" + str(idx) + "\" \"$STAGE" + dest_path + "\"",
        )

    # (2) data trees (filegroups/globs) → staged under prefix preserving the
    #     file's workspace-relative short_path (minus any strip_prefix).
    for f in ctx.files.data:
        rel = f.short_path
        if ctx.attr.strip_prefix and rel.startswith(ctx.attr.strip_prefix):
            rel = rel[len(ctx.attr.strip_prefix):].lstrip("/")
        dest = ctx.attr.prefix.rstrip("/") + "/" + rel
        idx = len(src_files)
        src_files.append(f)
        copy_cmds.append(
            "install -D -m " + _exec_mode(dest) +
            " \"$SRC" + str(idx) + "\" \"$STAGE" + dest + "\"",
        )

    deb_name = ctx.attr.package + "_" + ctx.attr.version + "_" + arch + ".deb"
    out_deb = ctx.actions.declare_file(deb_name)

    src_vars = []
    for i, f in enumerate(src_files):
        src_vars.append("SRC" + str(i) + "='" + f.path + "'")

    def _script_block(name, body):
        if not body:
            return ""
        return (
            "cat > \"$CTRL/" + name + "\" << '__" + name.upper() + "_EOF__'\n" +
            body + "\n__" + name.upper() + "_EOF__\n" +
            "chmod 755 \"$CTRL/" + name + "\"\n"
        )

    # Installed-Size is in KiB of the staged data tree (dpkg convention).
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
        "INSTALLED_KB=$(du -sk \"$STAGE\" | cut -f1)\n" +
        # control rendered at action time so Installed-Size reflects the stage.
        "cat > \"$CTRL/control\" << __CONTROL_EOF__\n" +
        _render_control(ctx.attr, arch, "$INSTALLED_KB") +
        "__CONTROL_EOF__\n" +
        _script_block("postinst", ctx.attr.postinst) +
        _script_block("prerm", ctx.attr.prerm) +
        _script_block("conffiles", ctx.attr.conffiles) +
        "( cd \"$STAGE\" && tar czf \"$TMP/data.tar.gz\" . )\n" +
        "( cd \"$CTRL\"  && tar czf \"$TMP/control.tar.gz\" . )\n" +
        "printf '2.0\\n' > \"$TMP/debian-binary\"\n" +
        "ar cr '" + out_deb.path + "' \"$TMP/debian-binary\" " +
        "\"$TMP/control.tar.gz\" \"$TMP/data.tar.gz\"\n" +
        "echo 'Built: " + deb_name + "'\n"
    )

    ctx.actions.run_shell(
        command = script,
        inputs = depset(src_files),
        outputs = [out_deb],
        mnemonic = "PkgDeb",
        progress_message = "PkgDeb " + deb_name,
        execution_requirements = {"no-sandbox": "1"},
    )
    return [DefaultInfo(files = depset([out_deb]))]

pkg_deb = rule(
    implementation = _pkg_deb_impl,
    attrs = {
        "package": attr.string(mandatory = True),
        "version": attr.string(default = "0.1.0"),
        # Default amd64; pass arch="aarch64"/"arm64" or "all" (python pkgs).
        "arch": attr.string(default = "amd64"),
        "section": attr.string(default = "misc"),
        "priority": attr.string(default = "optional"),
        "maintainer": attr.string(default = "Theia <theia@robofortis.com>"),
        "description": attr.string(mandatory = True),
        "depends": attr.string(default = ""),
        "provides": attr.string(default = ""),
        # Single-file → absolute dest (binaries, scripts, configs).
        "files": attr.label_keyed_string_dict(allow_files = True),
        # Source/header trees staged under `prefix`, preserving short_path
        # (minus `strip_prefix`).
        "data": attr.label_list(allow_files = True),
        "prefix": attr.string(default = "/opt/theia"),
        "strip_prefix": attr.string(default = ""),
        "postinst": attr.string(default = ""),
        "prerm": attr.string(default = ""),
        "conffiles": attr.string(default = ""),
    },
)
