"""opkg.bzl — BACK-COMPAT shim. `pkg_opkg` is now pkg_deb(format="ipk").

A .deb and an opkg .ipk are the same ar(1) archive (debian-binary +
control.tar.gz + data.tar.gz), so Theia has ONE packaging rule — `pkg_deb` in
rules/deb.bzl — that emits either format. `pkg_opkg` is retained here only so
existing `load("//rules:opkg.bzl", "pkg_opkg")` callers keep working; it forwards
to pkg_deb with format="ipk". Prefer `pkg_deb` directly (Theia is always on
Debian-derived platforms; .ipk is the opt-in hatch).
"""

load("//rules:deb.bzl", _pkg_opkg = "pkg_opkg")

# Explicit re-export so `load("//rules:opkg.bzl", "pkg_opkg")` keeps working.
pkg_opkg = _pkg_opkg
