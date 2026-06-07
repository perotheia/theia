# theia::provisioning — Phase 1: the cheap, stable base. Machine-generic.
#
# Provisioning installs ONLY etcd (one per cluster) + drops an EMPTY executor.json
# so the supervisor can boot. The per-machine .ipk (supervisor + FC/app binaries)
# and the REAL configs are theia::orchestration's job (Phase 2).
#
#   1. etcd — ONE per cluster, on $etcd_machine (default 'central'). In docker
#      etcd is a separate compose service / the host's etcd, so a container told
#      THEIA_ETCD_EXTERNAL (→ FACTER_theia_etcd_external) SKIPS the install.
#   2. EMPTY executor.json (root node, no children) at /opt/theia/config/ — the
#      supervisor boots and supervises nothing. The provisioning checkpoint:
#      `tdb ps` shows the bare root tree on every machine.
#
# Deliberately does NOT parse machine.json — it needs no per-host JSON (etcd is
# decided by $machine == $etcd_machine; the empty tree is static). That keeps
# provisioning free of a stdlib parsejson() dependency. $machine comes from
# FACTER_theia_machine.

class theia::provisioning (
    String $machine,
    String $etcd_machine = 'central',
) {
    file { ['/opt/theia', '/opt/theia/config']:
        ensure => directory,
        mode   => '0755',
    }

    notice("theia::provisioning: machine '${machine}' (etcd host: ${etcd_machine})")

    # ----- 1. etcd (one per cluster) --------------------------------------
    # Install only on the etcd machine, and only when etcd isn't externally
    # provided (docker compose etcd service / the host's etcd).
    $etcd_external = $facts['theia_etcd_external'] != undef
    if $machine == $etcd_machine and !$etcd_external {
        package { 'etcd-server':
            ensure => 'installed',
        }
    }

    # ----- 2. EMPTY executor.json — supervisor boots, supervises nothing --
    # `replace => false`: never clobber a real tree orchestration wrote; this
    # only SEEDS the empty one on a greenfield machine.
    file { '/opt/theia/config/executor.json':
        ensure  => 'file',
        mode    => '0644',
        replace => false,
        content => epp('theia/executor-empty.json.epp', {}),
        require => File['/opt/theia/config'],
    }
}
