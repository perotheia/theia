# orchestration.pp — Phase 2 site manifest.
#
# Apply with (or just `theia orchestrate`):
#   puppet apply --modulepath=deploy/puppet/modules \
#                deploy/puppet/orchestration.pp \
#                --hiera_config=deploy/puppet/hiera.yaml
#
# Resolves the target machine from $THEIA_MACHINE env (falling back
# to the hostname). Drives theia::orchestration, which reads
# /etc/theia/manifest/application.json and reinstalls the .ipks
# under /opt/theia/apps/.
#
# WHEN TO USE: day-to-day application pushes. Does NOT touch the
# supervisor / gateway / schema. Safe to run on a live system
# without service downtime — the supervisor reloads its child
# table after the .ipk swap.
#
# NOT for supervisor / gateway / schema updates — those go through
# provisioning.pp (full reprovisioning + offline migration).

$machine = $facts['theia_machine'] ? {
    undef   => $facts['hostname'],
    default => $facts['theia_machine'],
}

notice("orchestration.pp: applying theia::orchestration for machine '${machine}'")

class { 'theia::orchestration':
    machine => $machine,
}
