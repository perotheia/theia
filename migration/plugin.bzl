"""Per-node config-migration plugin macro.

Each migration plugin is a standalone .so the running per dlopen's at
MigrateBulk time. It MUST be self-contained (per dlopen's with RTLD_NOW and
exports neither nanopb nor the proto descriptors), so each compiles demo.pb.c
IN, links nanopb statically, and uses a header-only proto dep (no runtime
shared lib / DT_NEEDED). Target name is node-keyed so the RF migration library
can build/load the right one per MigrationCase.
"""

load("@rules_cc//cc:defs.bzl", "cc_binary")

def migration_plugin(name, src, demo_hdr = ":demo_pb_hdr"):
    cc_binary(
        name = "libper_migrate_%s.so" % name,
        srcs = [src, "//platform/proto/system/demo:demo_pb_c"],
        copts = ["-std=c++17", "-fPIC"],
        linkshared = True,
        linkstatic = False,
        linkopts = ["-l:libprotobuf-nanopb.a"],
        deps = [
            "//services/per/impl:migration_registry",  # header-only C ABI
            demo_hdr,                                  # demo.pb.h include
        ],
    )
