# central.pp — Puppet manifest for the `central` container.
#
# Hosts: services (the 18 Adaptive AUTOSAR Functional Clusters) +
# gateway. The supervisor on this machine runs the OTP tree shared
# with compute; only the processes whose host_machine binding is
# `central_host` actually have binaries locally.
#
# Selected via run-supervisor.sh by `HOSTNAME` env var (compose sets
# it to `central`).

class { 'theia':
    machine       => 'central_host',
    ipk_path      => '/opt/theia/ipk/central_host.ipk',
    executor_json => '/opt/theia/ipk/executor.json',
    machines_yaml => '/opt/theia/ipk/machines.yaml',
}
