# theia::provisioning — Phase 1 of the two-phase deploy model.
#
# When this runs:
#   - First install of an ECU (greenfield)
#   - Full update: supervisor / gateway version bump
#   - Schema-version bump (services/per offline migration; deferred)
#
# What it does:
#   1. Install OS-level packages declared in machine.json.machine.os_packages
#      (apt on Ubuntu HostMachines, opkg on Yocto/OpenWrt targets).
#   2. Install Theia opkg artifacts (supervisor, gateway) named in
#      machine.json.machine.opkg_artifacts under /opt/theia/, drop their
#      systemd units, enable + start the service.
#   3. Apply the file-capability contract (theia::postinstall) at /opt/theia.
#   4. (Deferred) services/per offline schema migration on a schema delta.
#
# Per-machine config source: ${manifest_dir}/machine.json — emitted by
#   `artheia generate-manifest` and shipped in the Distribution tarball.
#
# Orchestration (Phase 2) is a SEPARATE class and a SEPARATE Puppet
# invocation: it touches only the app .ipks and never the supervisor /
# gateway. Use `theia::orchestration` for that.

class theia::provisioning (
    String $machine,
    String $manifest_dir = "/etc/theia/manifest",
    String $ipk_path     = "/opt/theia/ipk/${machine}.ipk",
) {
    $machine_json_path = "${manifest_dir}/machine.json"

    file { $manifest_dir:
        ensure => directory,
        mode   => '0755',
    }

    # Install the per-machine bundle (demo-<machine>.ipk). This drops every
    # binary — the FC daemons AND the supervisor — at /opt/theia/bin/<name>.
    # The opkg_artifacts loop below only adds systemd units on top; there is
    # NO standalone supervisor.ipk (the supervisor rides in this bundle).
    class { 'theia':
        machine  => $machine,
        ipk_path => $ipk_path,
    }

    # Parse the per-machine manifest. parsejson() is a core Puppet function
    # (no module dep); the file is staged by the Distribution tarball.
    $manifest = parsejson(file($machine_json_path))
    $spec     = $manifest['machine']

    notice("theia::provisioning: ${machine_json_path} → machine '${machine}'")

    # ----- 1. OS packages (apt / opkg) ------------------------------------
    $spec['os_packages'].each |$pkg| {
        package { $pkg['name']:
            ensure   => $pkg['version'] ? {
                ''      => 'installed',
                undef   => 'installed',
                default => $pkg['version'],
            },
            provider => $pkg['source'],   # apt | opkg
        }
    }

    # ----- 2. Theia opkg artifacts — systemd units ------------------------
    #
    # The artifact BINARIES (supervisor, gateway) are NOT installed as
    # standalone .ipks here — they ride in the per-machine bundle
    # (demo-<machine>.ipk) that `theia::install` dpkg-installs, landing at
    # ${target_dir}/<name> (= /opt/theia/bin/<name>). Provisioning's job for
    # each artifact is to drop its systemd UNIT (ExecStart points at that
    # path) and enable the service. So this loop requires Class['theia::
    # install'] (the bundle), not a per-artifact package{}.
    $spec['opkg_artifacts'].each |$art| {
        file { $art['systemd_unit']:
            ensure  => 'file',
            content => epp('theia/theia-unit.service.epp', {
                'name'       => $art['name'],
                'target_dir' => $art['target_dir'],
            }),
            require => Class['theia::install'],
            notify  => Service["theia-${art['name']}"],
        }

        service { "theia-${art['name']}":
            ensure  => 'running',
            enable  => $art['enable_on_boot'],
            require => File[$art['systemd_unit']],
        }
    }

    # ----- 2b. File capabilities (setcap) ---------------------------------
    #
    # The cap CONTRACT lives once in theia::postinstall (shared with the local
    # dev path); apply it at the deploy root AFTER the bundle is installed.
    class { 'theia::postinstall':
        root    => '/opt/theia/bin',
        require => Class['theia::install'],
    }

    # ----- 3. services/per schema migration (DEFERRED) --------------------
    #
    # Until the migrate CLI lands, this is a marker. Intended shape:
    #   exec { 'theia-per-migrate':
    #       command => '/opt/theia/bin/tdb migrate --apply',
    #       onlyif  => '/opt/theia/bin/tdb migrate --check',
    #       require => Service['theia-supervisor'],
    #   }
    notice("theia::provisioning: services/per migration step is DEFERRED")
}
