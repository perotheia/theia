"""patched_source_tree — copy a source tree + apply a patch, as ONE directory.

rules_foreign_cc's cmake() needs `lib_source` to resolve to a single source
ROOT. A genrule can't emit a directory, and a mixed filegroup (real files +
bazel-out patched files) gives foreign_cc two roots → detect_root fails. So this
rule declares ONE output TreeArtifact, copies the input tree into it, and applies
the patch in-place. The vendored submodule on disk stays pristine; the .patch is
the sole source of the delta.
"""

def _impl(ctx):
    out = ctx.actions.declare_directory(ctx.attr.out_dir)
    # The input tree's root = the DIR holding the strip_prefix marker file
    # (e.g. third_party/etcd-cpp-apiv3, from .../etcd-cpp-apiv3/CMakeLists.txt).
    src_root = ctx.file.strip_prefix.dirname
    ctx.actions.run_shell(
        inputs = ctx.files.srcs + [ctx.file.patch],
        outputs = [out],
        command = (
            "set -e\n" +
            "src='%s'\n" % src_root +
            "out='%s'\n" % out.path +
            "patch_file='%s'\n" % ctx.file.patch.path +
            # copy the whole source tree into the single output dir.
            # -L dereferences: bazel feeds source files as symlinks into a
            # read-only store; patch refuses to edit symlinks, so we need real
            # regular files in the output. -p strips write-protect bits.
            "cp -aL \"$src/.\" \"$out/\"\n" +
            "chmod -R u+w \"$out\"\n" +
            # apply the patch inside it (paths in the patch are a/<f> b/<f>)
            "patch -p1 -d \"$out\" < \"$patch_file\"\n"
        ),
        mnemonic = "PatchTree",
        progress_message = "Patching %s with %s" % (src_root, ctx.file.patch.basename),
    )
    return [DefaultInfo(files = depset([out]))]

patched_source_tree = rule(
    implementation = _impl,
    attrs = {
        "srcs": attr.label(mandatory = True, allow_files = True,
                           doc = "filegroup of the source tree to patch"),
        "strip_prefix": attr.label(allow_single_file = True,
                                   doc = "a file at the source root; its dir is the copy root"),
        "patch": attr.label(mandatory = True, allow_single_file = True),
        "out_dir": attr.string(mandatory = True,
                               doc = "name of the output TreeArtifact directory"),
    },
)
