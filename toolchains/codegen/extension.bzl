"""Module extension wrapping the @theia_codegen repository rule (bzlmod).

Exposes the content-hashed x86 proto codegen toolset so the proto genrules can
declare protoc / grpc_cpp_plugin as `tools` instead of resolving them off the
host PATH (the non-hermeticity that shipped a stale descriptor).
"""

load(":theia_codegen.bzl", "theia_codegen")

def _impl(_module_ctx):
    theia_codegen(name = "theia_codegen")

theia_codegen_ext = module_extension(implementation = _impl)
