"""Module extension wrapping the aarch64 sysroot repository rules (bzlmod)."""

load(":rpi4_sysroot.bzl", "orin_sysroot", "rpi4_sysroot")

def _impl(_module_ctx):
    rpi4_sysroot(name = "rpi4_sysroot")
    orin_sysroot(name = "orin_sysroot")

rpi4_sysroot_ext = module_extension(implementation = _impl)
