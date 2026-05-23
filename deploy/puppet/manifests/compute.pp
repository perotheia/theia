# compute.pp — Puppet manifest for the `compute` container.
#
# Hosts: demo binaries (Demo3Way's three per-process compositions).
# The supervisor on this machine spawns those binaries locally; FCs
# referenced by the shared OTP tree but bound to central_host are
# satisfied remotely (or, today, skipped — they'll get a "module
# missing" log).

class { 'theia':
    machine       => 'compute_host',
    ipk_path      => '/opt/theia/ipk/compute_host.ipk',
    executor_yaml => '/opt/theia/ipk/executor.yaml',
    machines_yaml => '/opt/theia/ipk/machines.yaml',
}
