"""Repository rule that captures the staged x86 proto codegen toolset
(third_party/codegen-bookworm-x86) as CONTENT-HASHED bazel inputs.

WHY THIS EXISTS
---------------
The proto genrules used to shell out to a host/.venv `protoc` /
`nanopb_generator` with `tools = []` + `local = True`. Only the PATH *string*
(via `--action_env=PATH`) entered the action cache key, never the generator
BINARIES' content. So a generator/.venv change (or a stale-tree local run)
could serve codegen keyed on an unchanged key — which once dropped a freshly
added proto field (supervisor.SystemInfo.release_version, tag 15) on an
incremental build and shipped a stale C++ descriptor. See
[[project-com-getsysteminfo-heisenbug]] / [[feedback-stop-and-fix-main]].

By SYMLINKING the staged protoc + grpc_cpp_plugin + their runtime libs into an
external repo and exposing them as real bazel files, the genrules can declare
them in `tools` (or `srcs`): their content is now in the cache key, the action
sandboxes (no `local = True`), and a generator change correctly invalidates.

The staged toolset is x86 protoc 3.21.12 (matches the bookworm libprotobuf the
.pb.cc link against, host AND arm64) — using it for the host build too drops the
old /usr/bin/protoc 3.12.4 version skew.

Override the staged dir with THEIA_CODEGEN_DIR (e.g. a CI cache path).
"""

def _theia_codegen_impl(rctx):
    override = rctx.os.environ.get("THEIA_CODEGEN_DIR", "")
    if override:
        src = override
    else:
        src = str(rctx.workspace_root) + "/third_party/codegen-bookworm-x86"

    # Symlink the staged x86 codegen tree IN (so bazel hashes the binaries/libs)
    # — but ONLY if it exists. The rpi4 CROSS-build (on the x86 host) has it; a
    # NATIVE arm64 build (the Jetson) does NOT (it uses its own /usr/local protoc
    # off PATH and takes the genrule's default branch, never referencing :protoc).
    # Symlinking a missing path leaves a DANGLING link that breaks the glob, so
    # guard on existence.
    if rctx.path(src + "/bin").exists:
        rctx.symlink(src + "/bin", "bin")
    if rctx.path(src + "/lib").exists:
        rctx.symlink(src + "/lib", "lib")

    # Also content-hash the nanopb GENERATOR package (the .venv install). The
    # nanopb genrules still invoke `nanopb_generator` off PATH, but listing this
    # filegroup in their `srcs` puts the generator's CODE in the action cache key
    # — so a generator/.venv upgrade correctly invalidates (the hole that shipped
    # a stale descriptor). The venv python version varies by box (3.10 on the x86
    # dev box, 3.8 on the focal Jetson), so probe a few. Override with
    # THEIA_NANOPB_GEN_DIR. Missing → an empty filegroup (the generator runs off
    # PATH regardless; only the cache-key hashing is skipped).
    nbsrc = rctx.os.environ.get("THEIA_NANOPB_GEN_DIR", "")
    if not nbsrc:
        ws = str(rctx.workspace_root)
        for py in ["python3.10", "python3.8", "python3.9", "python3.11", "python3.12"]:
            cand = ws + "/.venv/lib/" + py + "/site-packages/nanopb/generator"
            if rctx.path(cand).exists:
                nbsrc = cand
                break
    if nbsrc and rctx.path(nbsrc).exists:
        rctx.symlink(nbsrc, "nanopb_generator")

    rctx.file("BUILD.bazel", _BUILD)

# protoc/grpc_cpp_plugin need their staged .so's on LD_LIBRARY_PATH at run time;
# we expose the whole lib/ tree as runfiles data so a genrule that lists these
# tools also stages the libs. The genrule cmd sets LD_LIBRARY_PATH to the lib
# dir (resolved from the tool location).
_BUILD = """\
package(default_visibility = ["//visibility:public"])

filegroup(
    name = "libs",
    srcs = glob(["lib/**"], allow_empty = True),
)

# glob (allow_empty) not a hard src: on a NATIVE arm64 build there is no staged
# bin/ (the Jetson uses /usr/local protoc off PATH, never referencing these), so
# the filegroup must resolve EMPTY rather than error on a missing bin/protoc.
filegroup(
    name = "protoc",
    srcs = glob(["bin/protoc"], allow_empty = True),
    data = [":libs"],
)

filegroup(
    name = "grpc_cpp_plugin",
    srcs = glob(["bin/grpc_cpp_plugin"], allow_empty = True),
    data = [":libs"],
)

# The nanopb generator package source — list in a nanopb genrule's `srcs` so
# its CODE enters the action cache key (generator still runs off PATH).
filegroup(
    name = "nanopb_generator_src",
    srcs = glob(["nanopb_generator/**"], allow_empty = True),
)

# Everything (protoc + plugin + libs) as one exec-input bundle for genrules.
filegroup(
    name = "all",
    srcs = [":protoc", ":grpc_cpp_plugin", ":libs"],
)
"""

theia_codegen = repository_rule(
    implementation = _theia_codegen_impl,
    environ = ["THEIA_CODEGEN_DIR"],
    local = True,
)
