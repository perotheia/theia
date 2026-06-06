# theia::service — supervisor lifecycle.
#
# In the dev compose, systemd-as-PID-1 inside Docker is impractical
# without --privileged. So the supervisor runs in foreground from
# run-supervisor.sh (which is the container entrypoint, exec'd
# AFTER Puppet apply returns).
#
# This class is a placeholder for the production path where the
# container runs systemd and the supervisor is a real
# systemd unit:
#
#   file { '/etc/systemd/system/theia-supervisor.service':
#       ensure  => file,
#       content => epp('theia/theia-supervisor.service.epp'),
#       notify  => Service['theia-supervisor'],
#   }
#
#   service { 'theia-supervisor':
#       ensure => running,
#       enable => true,
#   }
#
# For now this class is intentionally empty — run-supervisor.sh handles
# the lifecycle directly. Puppet's idempotency stops at file drops
# (executor.json, .ipk install).

class theia::service {
    # Intentionally empty. The supervisor is started by
    # run-supervisor.sh in foreground after Puppet returns. See
    # docs/deploy.md for the rationale.
    notice("theia::service: supervisor lifecycle handled by run-supervisor.sh (foreground exec)")
}
