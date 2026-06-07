# theia::orchestration — Phase 2: install the binaries + the REAL config.
# Machine-generic (reads <machine>/{application,execution}.json).
#
# Provisioning (Phase 1) left an empty executor.json and (on the etcd host) etcd.
# Orchestration does the frequent work:
#   1. Install the per-machine .ipk bundle → /opt/theia/bin/<name> (supervisor +
#      FC/app binaries). dpkg (opkg on a Yocto target).
#   2. setcap the binaries (theia::postinstall) — caps clear on a fresh copy.
#   3. Write the REAL executor.json from execution.json.supervisor_tree (replaces
#      the empty tree provisioning seeded) + machines.json.
#   4. Notify the supervisor to reload (tdb) — or, in the docker flow, the
#      entrypoint restart picks up the new executor.json.
#
# $machine from FACTER_theia_machine; reads ${manifest_dir}/<machine>/*. No
# per-host .pp.

class theia::orchestration (
    String $machine,
    String $manifest_dir = "/etc/theia/manifest",
    String $ipk_dir      = "/opt/theia/ipk",
    String $tdb_bin      = "/opt/theia/bin/tdb",
) {
    $executor_src = "${manifest_dir}/${machine}/executor.json"
    $ipk_path     = "${ipk_dir}/${machine}.ipk"

    notice("theia::orchestration: ${machine} ← ${ipk_path} + real executor.json")

    file { ['/opt/theia', '/opt/theia/config']:
        ensure => directory,
        mode   => '0755',
    }

    # ----- 1. Install the per-machine .ipk bundle -------------------------
    # The bundle drops every binary (supervisor + FCs) at /opt/theia/bin/<name>.
    # dpkg handles the opkg ar+tar.gz archive; switch to provider opkg on Yocto.
    package { "theia-${machine}":
        ensure   => 'latest',
        provider => 'dpkg',
        source   => $ipk_path,
    }

    # ----- 2. setcap (caps clear on a fresh binary copy) ------------------
    class { 'theia::postinstall':
        root    => '/opt/theia/bin',
        require => Package["theia-${machine}"],
    }

    # ----- 3. REAL executor.json (the supervisor tree) --------------------
    # Replaces the empty tree provisioning seeded. `theia manifest` already
    # extracted execution.json.supervisor_tree into a ready-to-run executor.json
    # per host, so puppet just COPIES it (no JSON re-serialize / stdlib needed).
    file { '/opt/theia/config/executor.json':
        ensure  => 'file',
        mode    => '0644',
        source  => $executor_src,
        require => [File['/opt/theia/config'], Package["theia-${machine}"]],
        notify  => Exec['theia-supervisor-reload'],
    }

    # ----- 4. Reload the supervisor (no blanket restart) ------------------
    # Fires only when executor.json changed, and ONLY if tdb is present — on a
    # live host tdb reload is the no-downtime path; in docker tdb isn't packaged
    # yet and the entrypoint restart re-reads the config instead, so a missing
    # tdb must not fail the catalog.
    exec { 'theia-supervisor-reload':
        command     => "${tdb_bin} reload",
        refreshonly => true,
        onlyif      => "/usr/bin/test -x ${tdb_bin}",
        path        => ['/usr/bin', '/bin', '/opt/theia/bin'],
    }
}
