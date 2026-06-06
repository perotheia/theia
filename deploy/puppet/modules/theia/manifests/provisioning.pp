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
) {
    $machine_json_path = "${manifest_dir}/machine.json"

    file { $manifest_dir:
        ensure => directory,
        mode   => '0755',
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

    # ----- 2. Theia opkg artifacts under /opt/theia/ ----------------------
    #
    # Each artifact's .ipk was built by `bazel build <bazel_target>` (the
    # rig packages it) and shipped in /opt/theia/ipk/. Install it, drop the
    # systemd unit, enable + start. The supervisor/gateway live here.
    $spec['opkg_artifacts'].each |$art| {
        $art_ipk = "/opt/theia/ipk/${art['name']}.ipk"

        # dpkg --install: the pkg_opkg archive is .deb-compatible on amd64.
        # On a Yocto/OpenWrt target switch provider to 'opkg'.
        package { "theia-${art['name']}":
            ensure   => 'installed',
            provider => 'dpkg',
            source   => $art_ipk,
        }

        file { $art['systemd_unit']:
            ensure  => 'file',
            content => epp('theia/theia-unit.service.epp', {
                'name'       => $art['name'],
                'target_dir' => $art['target_dir'],
            }),
            require => Package["theia-${art['name']}"],
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
    # dev path); apply it at the deploy root AFTER the artifacts are installed.
    class { 'theia::postinstall':
        root    => '/opt/theia',
        require => Package[$spec['opkg_artifacts'].map |$a| { "theia-${a['name']}" }],
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
