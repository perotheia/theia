# theia::install — opkg-install the per-machine .ipk.
#
# The .ipk arrives via docker-compose volume mount at
# /opt/theia/ipk/<machine>.ipk. We opkg-install it into the container
# root filesystem (this is the standard "opkg install <file>" form,
# not a fetch-from-feed install).
#
# The .ipk's data.tar.gz drops /usr/bin/<component> binaries and
# (eventually) /usr/bin/theia-supervisor. Today the supervisor binary
# isn't yet packaged as part of the rig .ipk — we bind-mount it from
# the workspace build instead. That bind-mount is configured in
# docker-compose.yml; this Puppet class only installs the rig's
# components into /usr/bin via opkg.

class theia::install {
    $machine = $theia::machine
    $ipk_path = $theia::ipk_path

    # Three possible states for the .ipk:
    #   - File present, non-empty → real package, opkg-install it.
    #   - File present, zero bytes → empty marker (stage.sh writes
    #     this when the machine has no bazel_buildable components).
    #     Skip opkg-install; the supervisor will run with no
    #     locally-installed apps.
    #   - File missing → stage.sh didn't run, or the rig.json doesn't
    #     name this machine. Fail loudly.

    exec { 'theia::install::check-ipk':
        command => "/bin/sh -c 'test -f ${ipk_path}'",
        unless  => "/bin/sh -c 'test -f ${ipk_path}'",
        path    => '/usr/bin:/bin',
    }

    # Install only when the .ipk has actual content. Zero-byte
    # files mean "no buildable components for this machine"; skip
    # gracefully so central can come up even with no FC binaries yet.
    #
    # We invoke `dpkg -i` (not `opkg install`). The pkg_opkg rule emits
    # an ar+control.tar.gz+data.tar.gz archive that is byte-for-byte
    # the same as a .deb when arch is `amd64`. On an OpenWrt / Yocto
    # opkg target, switch this exec to `opkg install` — the archive
    # is compatible either way.
    exec { 'theia::install::dpkg':
        command   => "/usr/bin/dpkg --install --force-overwrite ${ipk_path}",
        onlyif    => "/bin/sh -c '[ -s ${ipk_path} ]'",   # -s: file > 0 bytes
        require   => Exec['theia::install::check-ipk'],
        logoutput => true,
    }

    # Log "skipped" to stdout when the .ipk is empty. Cheap exec so
    # the operator sees the reason in `docker logs`.
    # ASCII-only message string — Puppet's exec result handling
    # rejects non-ASCII bytes when LANG is unset (default in docker).
    exec { "theia::install::${machine}::skip-empty":
        command => "/bin/sh -c 'echo \"[theia::install] skipping dpkg install: ${ipk_path} is empty (no bazel_buildable components for ${machine})\"'",
        onlyif  => "/bin/sh -c '[ -f ${ipk_path} ] && [ ! -s ${ipk_path} ]'",
        path    => '/usr/bin:/bin',
        logoutput => true,
    }
}
