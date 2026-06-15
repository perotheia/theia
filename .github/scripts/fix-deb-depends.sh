#!/usr/bin/env bash
# Recompute each .deb's runtime-library Depends on the ACTUAL build host using
# dpkg-shlibdeps, so the package names match THIS Ubuntu release (libprotobuf
# and libgrpc++ sonames differ across 22.04 / 24.04, and libcpprest is gone in
# 24.04). The packaging rule's hardcoded `Depends:` is treated as a fallback —
# we replace its system-lib entries with shlibdeps' resolution while preserving
# the inter-package deps (theia-runtime, theia-framework, …).
set -euo pipefail

UBUNTU="${1:-unknown}"
ROOT="$(pwd)"
shopt -s nullglob

for deb in dist/debian/*/*.deb; do
  pkg="$(dpkg-deb -f "$deb" Package)"
  # Only the binary-bearing packages have shared-lib deps worth resolving.
  case "$pkg" in
    theia-services|theia-runtime) ;;
    *) echo "skip $pkg (no compiled binaries)"; continue ;;
  esac

  work="$(mktemp -d)"
  dpkg-deb -R "$deb" "$work"

  # Collect ELF binaries staged under /opt/theia (bin + bundled .so).
  mapfile -t elfs < <(find "$work/opt/theia" -type f \
                        \( -path '*/bin/*' -o -name '*.so*' \) 2>/dev/null \
                      | while read -r f; do file "$f" | grep -q ELF && echo "$f"; done)

  sys_deps=""
  if [ "${#elfs[@]}" -gt 0 ]; then
    # dpkg-shlibdeps needs paths RELATIVE to the dir it runs in, and prints
    # `shlibs:Depends=...` to STDOUT with -O (warnings go to stderr). Run from
    # the extracted root; strip the "$work/" prefix off each ELF path.
    rels=(); for e in "${elfs[@]}"; do rels+=("${e#"$work"/}"); done
    mkdir -p "$work/debian"; : > "$work/debian/control"
    shlibs_out="$(cd "$work" && \
      LD_LIBRARY_PATH="opt/theia/lib" \
      dpkg-shlibdeps -O --ignore-missing-info -e "${rels[@]}" 2>/dev/null || true)"
    sys_deps="$(printf '%s\n' "$shlibs_out" | sed -n 's/^shlibs:Depends=//p' | head -1)"
  fi

  # Preserve the non-system (inter-package) deps from the original control:
  # everything that starts with theia-* or is a known build-time pin. The grep
  # may match nothing (e.g. theia-runtime has no Depends) — never let that abort
  # the script under `set -e`.
  # Split the ORIGINAL Depends on commas (the dep separator — version specs use
  # parens, never commas) and keep only the inter-package entries.
  orig="$(awk -F': ' '/^Depends:/{print $2}' "$work/DEBIAN/control")"
  keep="$(echo "$orig" | tr ',' '\n' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//' \
          | { grep -E '^(theia-|libnanopb-dev|build-essential)' || true; })"
  # sys_deps from shlibdeps is already a clean comma-list; split it the same way.
  sysd="$(echo "${sys_deps:-}" | tr ',' '\n' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"

  # Merge, drop blanks + dupes (by package name, keeping first), re-join with ", ".
  newdeps="$(printf '%s\n%s\n' "$keep" "$sysd" | { grep -vE '^$' || true; } \
             | awk '!seen[$1]++' | paste -sd',' | sed 's/,/, /g')"

  if [ -z "$newdeps" ]; then
    echo "[$UBUNTU] $pkg: no deps resolved — leaving control unchanged"
    rm -rf "$work"; continue
  fi

  echo "[$UBUNTU] $pkg Depends: $newdeps"
  # Rewrite the Depends line in the control file, then repack in place.
  sed -i "/^Depends:/d" "$work/DEBIAN/control"
  printf 'Depends: %s\n' "$newdeps" >> "$work/DEBIAN/control"
  dpkg-deb --build --root-owner-group "$work" "$deb" >/dev/null
  rm -rf "$work"
done

echo "fix-deb-depends: done for ubuntu $UBUNTU"
