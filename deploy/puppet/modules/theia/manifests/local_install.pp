# theia::local_install — populate a LOCAL install/<machine>/ tree from
# bazel-built binaries (the dev inner-loop equivalent of the remote .ipk
# deploy). Replaces demo/stage_local.sh's cp + setcap with the SAME puppet code
# path a real deploy uses, just with files copied from bazel-bin instead of
# unpacked from an .ipk, and dest = the workspace install/ instead of /opt.
#
# What it owns (NOT the build / executor.json emit — those stay in the `theia`
# command wrapper, which is bazel's job):
#   - the supervisor binary at <dest>/supervisor
#   - each child binary at <dest>/bin/<name>
#   - file capabilities (via theia::postinstall), applied AFTER the copies
#     (a fresh copy clears caps).
#
# Parameters:
#   dest           — the machine install dir (e.g. <repo>/install/central).
#   supervisor_src — absolute path to the bazel-built supervisor binary.
#   binaries       — { '<bin-name>' => '<abs src path>' } for each child. The
#                    bin-name is the executor.json start_cmd leaf (bin/<name>).
#
# Ordering: copies -> postinstall (setcap). The supervisor copy is the postinstall
# trigger anchor so caps re-apply whenever the binary changes.

class theia::local_install (
    String $dest,
    String $supervisor_src,
    Hash   $binaries = {},
) {
    file { $dest:
        ensure => directory,
        mode   => '0755',
    }
    file { "${dest}/bin":
        ensure  => directory,
        mode    => '0755',
        require => File[$dest],
    }

    # Supervisor binary at the machine root. `file{}` with source = the bazel
    # output: puppet copies it and re-copies on content change (so a rebuild is
    # picked up). mode 0755 (bazel-out is 0555/read-only; we want it runnable).
    file { "${dest}/supervisor":
        ensure  => file,
        source  => $supervisor_src,
        mode    => '0755',
        require => File[$dest],
    }

    # One child binary per entry.
    $binaries.each |$name, $src| {
        file { "${dest}/bin/${name}":
            ensure  => file,
            source  => $src,
            mode    => '0755',
            require => File["${dest}/bin"],
        }
    }

    # Caps — shared contract, at the LOCAL root. Run after all copies (a fresh
    # copy clears caps); File['supervisor'] notifies so caps re-apply on change.
    class { 'theia::postinstall':
        root    => $dest,
        require => File["${dest}/supervisor"],
    }
}
