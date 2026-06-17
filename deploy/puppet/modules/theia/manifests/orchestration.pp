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
    # `theia dist` builds <machine>.deb by default (the .ipk format is the opkg
    # hatch). Both are the SAME ar archive — dpkg installs either. Prefer the
    # .deb (the default `theia dist` output), fall back to .ipk for an opkg build.
    $deb_path = "${ipk_dir}/${machine}.deb"
    $ipk_path = "${ipk_dir}/${machine}.ipk"

    notice("theia::orchestration: ${machine} ← ${deb_path}|${ipk_path} + real executor.json")

    file { ['/opt/theia', '/opt/theia/config']:
        ensure => directory,
        mode   => '0755',
    }

    # ----- 1. Install the per-machine bundle (.deb preferred, .ipk fallback) --
    # The bundle drops every binary (supervisor + FCs) at /opt/theia/bin/<name>.
    # dpkg handles the ar+tar.gz archive (same format as opkg's .ipk). An explicit
    # exec (vs the package{} type) is robust against puppet's dpkg version-compare
    # on a re-converge and lets us pick whichever artifact `theia dist` produced.
    # PATH MUST include the sbin dirs: dpkg's postinst tooling shells out to
    # `ldconfig` (/usr/sbin) + `start-stop-daemon` (/usr/sbin); a /usr/bin-only
    # PATH makes dpkg abort with "expected programs not found in PATH".
    exec { "theia::orchestration::install::${machine}":
        command   => "/bin/sh -c 'f=\$( [ -f ${deb_path} ] && echo ${deb_path} || echo ${ipk_path} ); dpkg --install --force-overwrite \"\$f\"'",
        path      => ['/usr/local/sbin', '/usr/local/bin', '/usr/sbin', '/usr/bin', '/sbin', '/bin'],
        logoutput => true,
    }

    # ----- 2. setcap (caps clear on a fresh binary copy) ------------------
    class { 'theia::postinstall':
        root    => '/opt/theia/bin',
        require => Exec["theia::orchestration::install::${machine}"],
    }

    # ----- 3. REAL executor.json (the supervisor tree) --------------------
    # Replaces the empty tree provisioning seeded. `theia manifest` already
    # extracted execution.json.supervisor_tree into a ready-to-run executor.json
    # per host, so puppet just COPIES it (no JSON re-serialize / stdlib needed).
    file { '/opt/theia/config/executor.json':
        ensure  => 'file',
        mode    => '0644',
        source  => $executor_src,
        require => [File['/opt/theia/config'], Exec["theia::orchestration::install::${machine}"]],
        notify  => Exec['theia-supervisor-reload'],
    }

    # ----- 3b. Per-FC static config (the per-machine profiles) ------------
    # `theia manifest` emitted <machine>/config/<fc>.json (gen-params default +
    # deep-merged deploy/config/<machine>/<fc>.json override). Copy the whole dir
    # into /opt/theia/config/ so each FC's runtime config singleton resolves
    # $THEIA_CONFIG_DIR/<fc>.json (THEIA_CONFIG_DIR=config under THEIA_ROOT_DIR=
    # /opt/theia → /opt/theia/config/<fc>.json). THIS is what lands the tsync
    # GM-on-central / slave-on-compute split in the container; without it the FC
    # falls back to the .art slave default everywhere. recurse, but DON'T purge
    # (executor.json above lives here too). An absent source dir is tolerated
    # (older manifests with no config/) via the test-guarded exec, not file{}.
    $config_src = "${manifest_dir}/${machine}/config"
    exec { "theia::orchestration::config::${machine}":
        command   => "/bin/sh -c 'cp -f ${config_src}/*.json /opt/theia/config/ 2>/dev/null || true'",
        onlyif    => "/bin/sh -c 'ls ${config_src}/*.json >/dev/null 2>&1'",
        path      => ['/usr/bin', '/bin'],
        require   => File['/opt/theia/config'],
        logoutput => true,
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
