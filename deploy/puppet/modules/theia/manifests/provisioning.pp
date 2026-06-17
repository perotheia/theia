# theia::provisioning — Phase 1: the cheap, stable base. Machine-generic.
#
# Provisioning installs the per-host OS packages (etcd + the Linux daemons the
# FCs fork — ptp4l/phc2sys/chronyd/gpsd for tsync, …) + drops an EMPTY
# executor.json so the supervisor can boot. The per-machine .ipk (supervisor +
# FC/app binaries) and the REAL configs are theia::orchestration's job (Phase 2).
#
#   1. OS packages — from this host's machine.json `os_packages` (apt source).
#      These are the third-party Linux daemons the prebuilt Providers fork:
#      linuxptp (ptp4l + phc2sys), chrony, gpsd on central (PTP grandmaster),
#      linuxptp + chrony on compute (PTP slave). etcd is handled specially
#      below (one per cluster + the external-etcd hatch).
#   2. etcd — ONE per cluster, on $etcd_machine (default 'central'). In docker
#      etcd is a separate compose service / the host's etcd, so a container told
#      THEIA_ETCD_EXTERNAL (→ FACTER_theia_etcd_external) SKIPS the install.
#   3. EMPTY executor.json (root node, no children) at /opt/theia/config/ — the
#      supervisor boots and supervises nothing. The provisioning checkpoint:
#      `tdb ps` shows the bare root tree on every machine.
#
# $machine comes from FACTER_theia_machine. $manifest_dir is the artheia
# generate-manifest tree (mounted at /etc/theia/manifest in the docker flow);
# this host's machine.json is ${manifest_dir}/${machine}/machine.json. When the
# file is absent (a bare single-host bring-up with no emitted manifest), only the
# etcd special-case runs — no per-host package list is required.

class theia::provisioning (
    String $machine,
    String $etcd_machine = 'central',
    String $manifest_dir = '/etc/theia/manifest',
) {
    $machine_json = "${manifest_dir}/${machine}/machine.json"
    file { ['/opt/theia', '/opt/theia/config']:
        ensure => directory,
        mode   => '0755',
    }

    notice("theia::provisioning: machine '${machine}' (etcd host: ${etcd_machine})")

    # ----- 1. OS packages from machine.json -------------------------------
    # Install every apt-source os_package this host declares (the Linux daemons
    # the FCs fork). etcd-server is filtered out here and handled by the
    # cluster-singleton + external-etcd logic below; everything else (linuxptp,
    # chrony, gpsd, …) is a plain `package{ ensure => installed }`.
    if find_file($machine_json) {
        $os_packages = parsejson(file($machine_json))['machine']['os_packages']
        $apt_pkgs = $os_packages.filter |$p| {
            $p['source'] == 'apt' and $p['name'] != 'etcd-server'
        }.map |$p| { $p['name'] }
        notice("theia::provisioning: os_packages (apt) for '${machine}': ${apt_pkgs}")
        ensure_packages($apt_pkgs, { 'ensure' => 'installed' })
    }

    # ----- 2. etcd (one per cluster) --------------------------------------
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
