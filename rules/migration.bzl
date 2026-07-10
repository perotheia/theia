# Config-migration transform plugins — the .so per dlopen's at MigrateBulk time.
#
# The plugin carries the n→n+1 reshape code (artheia gen-transform output) and
# runs INSIDE the deployed (n) per, so it must be fully self-contained: it
# compiles the package's nanopb .pb.c IN and links nanopb statically — per
# dlopen's with RTLD_NOW and exports neither nanopb nor the proto descriptors,
# so the .so may have no DT_NEEDED on any proto/runtime shared lib. It speaks
# ONLY the C ABI in services/per/impl/migration_plugin_api.h (the
# :migration_registry dep is for that header alone, never per's C++ internals).
#
# A consuming workspace loads this as `@pero_theia//rules:migration.bzl`; the
# framework repo loads it the same way (the module name is pero_theia).
#
# The calling migrations/BUILD.bazel (scaffolded once by `artheia
# gen-migration`, preamble hand-owned) defines the two per-package defaults the
# managed entries lean on:
#
#   cc_library(name = "pb_hdr",                        # <pkg>.pb.h include path
#       hdrs = ["//proto/system/<pkg>:<leaf>_pb_h"],
#       strip_include_prefix = "/proto")
#   alias(name = "pb_c",                               # nanopb struct codecs
#       actual = "//proto/system/<pkg>:<leaf>_pb_c")
#
# so the managed block stays one minimal line per node:
#   migration_plugin(name = "<node>", src = "<node>_v1_to_v2.cc")
#
# `theia release-swp --migrate` cross-builds the entries with the SWP's --arch
# platform and ships each .so in the Mender artifact's migration/ part.

load("@rules_cc//cc:defs.bzl", "cc_binary")

def migration_plugin(name, src, pb_c = ":pb_c", pb_hdr = ":pb_hdr",
                     custom = None, deps = None):
    """One migration-transform plugin .so (libper_migrate_<name>.so).

    Args:
      name:   node key (e.g. "counter").
      src:    the gen-transform .cc (e.g. "counter_v1_to_v2.cc").
      pb_c:   nanopb .pb.c label compiled IN (default: the package ":pb_c"
              alias). None for a transform that only reshapes opaque bytes via
              a custom hook.
      pb_hdr: header-only cc_library giving the .pb.h include path (default:
              the package ":pb_hdr").
      custom: optional write-once custom-hook sidecar .cc (gen-transform emits
              it for {op:"custom"} rules).
      deps:   extra deps beyond the C-ABI header + pb_hdr.
    """
    srcs = [src]
    if custom:
        srcs.append(custom)
    if pb_c:
        srcs.append(pb_c)
    cc_binary(
        name = "libper_migrate_%s.so" % name,
        srcs = srcs,
        copts = ["-std=c++17", "-fPIC"],
        linkshared = True,
        linkstatic = False,
        linkopts = ["-l:libprotobuf-nanopb.a"],
        deps = ["@pero_theia//services/per/impl:migration_registry"] +
               ([pb_hdr] if pb_hdr else []) + (deps or []),
    )
