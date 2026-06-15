"""Module extension wrapping the @rpi4_sysroot repository rule (bzlmod)."""

load(":rpi4_sysroot.bzl", "rpi4_sysroot")

def _impl(_module_ctx):
    rpi4_sysroot(name = "rpi4_sysroot")

rpi4_sysroot_ext = module_extension(implementation = _impl)
