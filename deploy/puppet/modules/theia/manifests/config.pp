# theia::config — drop executor.yaml + machines.yaml into /etc/theia/.
#
# Each per-machine container gets its OWN executor.yaml (currently
# the rig has one global executor — see TODO/per-machine-executor.md).
# For now both machines read the same executor.yaml; the supervisor
# tree applies the SAME process list on each machine, with the
# expectation that each machine only runs the processes whose
# host_machine binding matches it.
#
# Future: artheia executor emit should grow a `--machine <name>` flag
# emitting only the slice of the supervisor tree relevant to that
# machine. Until then, both machines run the same tree; the redundant
# entries are harmless (the supervisor just doesn't see start_cmd
# files for processes not installed locally and either skips them or
# logs an error).

class theia::config {
    $executor_yaml = $theia::executor_yaml
    $machines_yaml = $theia::machines_yaml

    file { '/etc/theia':
        ensure => directory,
        mode   => '0755',
    }

    file { '/etc/theia/executor.yaml':
        ensure  => file,
        source  => $executor_yaml,  # bind-mounted from host
        mode    => '0644',
        require => File['/etc/theia'],
    }

    file { '/etc/theia/machines.yaml':
        ensure  => file,
        source  => $machines_yaml,
        mode    => '0644',
        require => File['/etc/theia'],
    }
}
