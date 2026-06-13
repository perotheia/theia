# theia::pkg_install — install the Theia PACKAGE SET (.deb / .ipk) on a target.
#
# The ROS2-style decomposition (theia release → dist/debian + dist/ipkg) lets a
# remote machine install just the packages it needs, in dependency order:
#
#     theia-framework  (apt — python wheel + rules; only where the user BUILDS)
#     theia-runtime    (supervisor binary + tombstone + runtime sources)
#     theia-services   (com/per/sm/ucm/log/shwa)         — OPTIONAL
#     <user apps>                                         — OPTIONAL
#
# This replaces the old per-machine single-.ipk install (theia::install) + the
# supervisor bind-mount: theia-runtime now PACKAGES the supervisor, so a target
# is self-contained from the deb/ipk set alone.
#
# Packages arrive under $pkg_dir (a docker volume / scp drop of dist/debian or
# dist/ipkg). Each is dpkg-installed if present; a missing OPTIONAL package is
# skipped (a runtime-only target needs no services). `dpkg --install` handles
# both .deb and .ipk (same ar format).
#
# Parameters:
#   pkg_dir   — dir holding the theia-*_*.{deb,ipk} files (default /opt/theia/pkg).
#   ext       — "deb" or "ipk" (which artifact set to install; default deb).
#   services  — install theia-services too? (default true)
#   app_globs — extra package globs to install AFTER services (the user's own
#               app debs/ipks), in order. Default []: runtime-only target.

class theia::pkg_install (
    String        $pkg_dir   = '/opt/theia/pkg',
    Enum['deb','ipk'] $ext   = 'deb',
    Boolean       $services  = true,
    Array[String] $app_globs = [],
) {
    # Ordered install: runtime first (everything depends on it), then services,
    # then the user's apps. dpkg resolves nothing from a feed here — these are
    # explicit local-file installs, so we sequence them ourselves.
    $base = [
        ['theia-runtime', true],            # required
        ['theia-services', $services],      # optional
    ]

    $base.each |$pair| {
        $pkg     = $pair[0]
        $enabled = $pair[1]
        if $enabled {
            # Install the single matching file (newest if several archs present —
            # the target's own arch is the only one staged in practice).
            # Accept both the flat drop (${pkg_dir}/<pkg>_*.ext) and the
            # `theia release` per-package subdir layout (${pkg_dir}/<pkg>/<pkg>_*.ext).
            exec { "theia::pkg_install::${pkg}":
                command   => "/bin/sh -c 'f=\$(ls -1 ${pkg_dir}/${pkg}_*.${ext} ${pkg_dir}/${pkg}/${pkg}_*.${ext} 2>/dev/null | head -1); [ -n \"\$f\" ] && dpkg --install --force-overwrite \"\$f\" || echo \"[theia::pkg_install] ${pkg} not present in ${pkg_dir} — skipped\"'",
                path      => '/usr/bin:/bin',
                logoutput => true,
            }
        }
    }

    # Sequence runtime → services so dpkg sees deps satisfied in order.
    if $services {
        Exec['theia::pkg_install::theia-runtime']
          -> Exec['theia::pkg_install::theia-services']
    }

    # User app packages, installed last (they depend on runtime [+services]).
    $app_globs.each |$glob| {
        exec { "theia::pkg_install::app::${glob}":
            command   => "/bin/sh -c 'for f in ${pkg_dir}/${glob}; do [ -f \"\$f\" ] && dpkg --install --force-overwrite \"\$f\"; done'",
            path      => '/usr/bin:/bin',
            logoutput => true,
            require   => $services ? {
                true    => Exec['theia::pkg_install::theia-services'],
                default => Exec['theia::pkg_install::theia-runtime'],
            },
        }
    }
}
