# theia::config — drop executor.json + machines.yaml into /etc/theia/.
#
# The supervisor tree is JSON-only since #380 (the supervisor binary
# parses JSON); $executor_json points at the executor.json emitted by
# @rig//:executor_json and staged into the image.
#
# Each per-machine container reads the same executor.json (the rig has
# one global executor tree); each machine only runs the processes whose
# host_machine binding matches it — redundant entries are harmless.

class theia::config {
    $executor_json = $theia::executor_json
    $machines_yaml = $theia::machines_yaml

    file { '/etc/theia':
        ensure => directory,
        mode   => '0755',
    }

    file { '/etc/theia/executor.json':
        ensure  => file,
        source  => $executor_json,  # bind-mounted from host
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
