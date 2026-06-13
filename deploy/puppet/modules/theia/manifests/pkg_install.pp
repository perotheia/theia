# theia::pkg_install — install the Theia PACKAGE SET (.deb / .ipk) on a target.
#
# The ROS2-style decomposition (theia release → dist/debian + dist/ipkg) lets a
# remote machine install just the packages it needs, in dependency order. The
# MACHINE packages are binary-only (no build artifacts); the -dev packages carry
# the sources/protos/manifest a WORKSPACE needs to build apps:
#
#     theia-framework      (apt — python wheel + rules; only where the user BUILDS)
#     theia-runtime        (supervisor binary)                    — required
#     theia-services       (com/per/sm/ucm/log/shwa + libetcd)    — OPTIONAL
#     theia-runtime-dev    (runtime sources/headers + protos)     — DEV only
#     theia-services-dev   (service protos + .art + py manifest)  — DEV only
#     <user apps>                                                  — OPTIONAL
#
# A pure DEPLOY target installs only the machine packages (theia-runtime
# [+ theia-services]) — zero build files. A target that also BUILDS apps adds the
# -dev packages (`dev => true`). theia-runtime PACKAGES the supervisor, so a
# target is self-contained from the machine set alone.
#
# Packages arrive under $pkg_dir (a docker volume / scp drop of dist/debian or
# dist/ipkg). Each is dpkg-installed if present; a missing OPTIONAL package is
# skipped. `dpkg --install` handles both .deb and .ipk (same ar format).
#
# Parameters:
#   pkg_dir   — dir holding the theia-*_*.{deb,ipk} files (default /opt/theia/pkg).
#   ext       — "deb" or "ipk" (which artifact set to install; default deb).
#   services  — install theia-services too? (default true)
#   dev       — also install the -dev packages (a build target)? (default false)
#   app_globs — extra package globs to install AFTER services (the user's own
#               app debs/ipks), in order. Default []: runtime-only target.

class theia::pkg_install (
    String        $pkg_dir   = '/opt/theia/pkg',
    Enum['deb','ipk'] $ext   = 'deb',
    Boolean       $services  = true,
    Boolean       $dev       = false,
    Array[String] $app_globs = [],
) {
    # Ordered install: runtime first (everything depends on it), then services,
    # then (if a build target) the -dev packages, then the user's apps. dpkg
    # resolves nothing from a feed — these are explicit local-file installs.
    $base = [
        ['theia-runtime', true],                      # required
        ['theia-services', $services],                # optional
        ['theia-runtime-dev', $dev],                  # DEV only
        ['theia-services-dev', $dev and $services],   # DEV only
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
    # -dev packages depend on their machine package — install after.
    if $dev {
        Exec['theia::pkg_install::theia-runtime']
          -> Exec['theia::pkg_install::theia-runtime-dev']
        if $services {
            Exec['theia::pkg_install::theia-services']
              -> Exec['theia::pkg_install::theia-services-dev']
        }
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
