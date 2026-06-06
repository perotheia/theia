# theia::config — drop executor.json + machines.json into /etc/theia/.
#
# The supervisor tree is JSON (the supervisor binary parses JSON); $executor_json
# points at the executor.json emitted by @rig//:executor_json and staged into the
# image. $machines_json is the GUI manifest (artheia emit-machines → machines.json).
#
# Each per-machine container reads the same executor.json (the rig has one global
# executor tree); each machine only runs the processes whose host_machine binding
# matches it — redundant entries are harmless.

class theia::config {
    $executor_json = $theia::executor_json
    $machines_json = $theia::machines_json

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

    file { '/etc/theia/machines.json':
        ensure  => file,
        source  => $machines_json,
        mode    => '0644',
        require => File['/etc/theia'],
    }
}
