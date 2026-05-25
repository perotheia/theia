# theia::init — convenience aggregator.
#
# Including `theia` (with the right parameters) wires up the full
# per-machine flow: install the .ipk, drop config, register the
# supervisor service. Per-host manifests (central.pp, compute.pp)
# `include` this class with their machine name set.
#
# Parameters:
#   machine        — Name as it appears in the rig (`central_host` /
#                    `compute_host`). Drives Puppet's lookup of the
#                    .ipk under /opt/theia/ipk/.
#   executor_json  — Absolute path on the host to the executor.json
#                    (supervisor tree, JSON-only since #380) that should
#                    land at /etc/theia/executor.json. Typically
#                    /opt/theia/ipk/executor.json because the
#                    docker-compose mounts it alongside the .ipk.
#   machines_yaml  — Same for machines.yaml (GUI manifest).
#   ipk_path       — Path to the per-machine .ipk inside the container.

class theia (
    String $machine,
    String $executor_json = "/opt/theia/ipk/executor.json",
    String $machines_yaml = "/opt/theia/ipk/machines.yaml",
    String $ipk_path      = "/opt/theia/ipk/${machine}.ipk",
) {
    include theia::install
    include theia::config
    include theia::service

    Class['theia::install']
      -> Class['theia::config']
      ~> Class['theia::service']
}
