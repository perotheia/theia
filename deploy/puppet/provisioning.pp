# provisioning.pp — Phase 1 site manifest.
#
# Apply with:
#   puppet apply --modulepath=deploy/puppet/modules \
#                deploy/puppet/provisioning.pp \
#                --hiera_config=/dev/null \
#                $(test -n "$THEIA_MACHINE" && echo "--node_terminus=plain")
#
# Resolves the target machine from $THEIA_MACHINE env (falling back
# to the hostname). Drives theia::provisioning, which reads
# /etc/theia/manifest/machine.yaml for the OS packages + opkg
# artifacts to install.
#
# WHEN TO USE: greenfield install, full update (supervisor / gateway
# / etcd bump), or any schema-delta release. Restarts the whole
# stack and (eventually) runs services/db offline migration.
#
# NOT for day-to-day app pushes — that's orchestration.pp.

$machine = $facts['theia_machine'] ? {
    undef   => $facts['hostname'],
    default => $facts['theia_machine'],
}

notice("provisioning.pp: applying theia::provisioning for machine '${machine}'")

class { 'theia::provisioning':
    machine => $machine,
}
