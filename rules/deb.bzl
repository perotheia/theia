"""deb.bzl — the ONE Theia packaging rule (`pkg_deb`).

A .deb and an opkg .ipk are the SAME on-disk format: an ar(1) archive of
debian-binary + control.tar.gz + data.tar.gz. dpkg installs both. So this single
rule emits either, switched by `format`:

  * format = "deb" (default) — output <package>_<version>_<arch>.deb, Debian arch
    names (amd64/arm64/all), control carries Installed-Size.
  * format = "ipk" — output <package>_<version>_<arch>.ipk, opkg arch names
    (x86_64/aarch64/all), leaner control (no Installed-Size). The opt-in "hatch"
    for an embedded/opkg target; everything else is the same archive.

Theia is always deployed on Debian-derived platforms (host/rpi4/docker, all
dpkg), so .deb is the default and primary; .ipk is one attr away if a non-Debian
target ever appears. `pkg_opkg` (rules/opkg.bzl) is kept as a thin alias =
pkg_deb(format="ipk") for back-compat.

`files` is the single-file → explicit-dest map (binaries, scripts); `data` +
`prefix` stages whole source/header trees preserving their workspace-relative
path. Both land in the same data.tar.gz.
"""

# Bazel CPU constraint value → arch name, per format.
_DEB_ARCH = {
    "x86_64": "amd64",
    "k8": "amd64",
    "aarch64": "arm64",
    "arm64": "arm64",
    "all": "all",
}
_IPK_ARCH = {
    "amd64": "x86_64",
    "k8": "x86_64",
    "x86_64": "x86_64",
    "arm64": "aarch64",
    "aarch64": "aarch64",
    "all": "all",
}

def _arch_for(fmt, raw):
    table = _IPK_ARCH if fmt == "ipk" else _DEB_ARCH
    return table.get(raw, raw)

def _render_control(attrs, arch, installed_kb, fmt):
    lines = [
        "Package: " + attrs.package,
        "Version: " + attrs.version,
        "Architecture: " + arch,
        "Section: " + attrs.section,
        "Priority: " + attrs.priority,
        "Maintainer: " + attrs.maintainer,
    ]
    # Installed-Size is a dpkg/.deb nicety; opkg control is leaner.
    if fmt == "deb":
        lines.append("Installed-Size: " + str(installed_kb))
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
    fmt = ctx.attr.format
    arch = _arch_for(fmt, ctx.attr.arch)

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

    ext = ".ipk" if fmt == "ipk" else ".deb"
    deb_name = ctx.attr.package + "_" + ctx.attr.version + "_" + arch + ext
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
        _render_control(ctx.attr, arch, "$INSTALLED_KB", fmt) +
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
        # "deb" (default, Debian arch names + Installed-Size) or "ipk" (opkg arch
        # names, leaner control). Same archive either way.
        "format": attr.string(default = "deb", values = ["deb", "ipk"]),
    },
)

def pkg_opkg(name, **kwargs):
    """Back-compat alias — an .ipk is pkg_deb(format="ipk"). Theia is always on
    Debian-derived platforms, so prefer plain pkg_deb (.deb); use this only for a
    non-Debian/opkg target. Drops the legacy `arch` default of x86_64 onto the
    rule's amd64 default, which _IPK_ARCH maps back to x86_64."""
    kwargs.pop("format", None)
    pkg_deb(name = name, format = "ipk", **kwargs)
