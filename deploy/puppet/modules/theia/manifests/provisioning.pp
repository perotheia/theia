# theia::provisioning — Phase 1 of the two-phase deploy model.
#
# When this runs:
#   - First install of an ECU (greenfield)
#   - Full update: supervisor / gateway / etcd version bump
#   - Schema-version bump (services/db will need offline migration)
#
# What it does:
#   1. Install OS-level packages declared in machine.yaml.os_packages
#      (apt on Ubuntu HostMachines, opkg on Yocto/OpenWrt targets).
#   2. Drop Theia-built opkg artifacts (theia-supervisor, theia-gateway)
#      under /opt/theia/, register their systemd units.
#   3. (Future) Run services/db --check; if a schema delta is detected,
#      stop running apps, run migration, restart.
#
# Per-machine config source: /etc/theia/manifest/machine.yaml
#   This file is written by `artheia generate-manifest --out dist/manifest`
#   and shipped to the target as part of the Distribution tarball.
#
# Orchestration (Phase 2) is a SEPARATE class and a SEPARATE Puppet
# invocation: it touches only the app .ipks and never the
# supervisor / gateway / etcd. Use `theia::orchestration` for that.

class theia::provisioning (
    String $machine,
    String $manifest_dir = "/etc/theia/manifest",
) {
    $machine_yaml_path = "${manifest_dir}/machine.yaml"

    # Defensive: bail loudly if the per-machine manifest dir is missing.
    # (The Distribution tarball drops these; running provisioning on a
    # machine with no manifest is operator error.)
    file { $manifest_dir:
        ensure => directory,
        mode   => '0755',
    }

    notice("theia::provisioning: reading ${machine_yaml_path} for machine '${machine}'")

    # ----- 1. OS packages (apt / opkg) ------------------------------------
    #
    # The real implementation parses machine.yaml and instantiates one
    # `package { … }` resource per `os_packages` entry. The `source`
    # field selects the provider (apt | opkg | pip).
    #
    # Today this is a STUB — Puppet's `loadyaml()` is a parser
    # function that needs a fully-typed read; production-ready
    # invocation looks like:
    #
    #   $machine_yaml = loadyaml($machine_yaml_path)
    #   $machine_yaml['machine']['os_packages'].each |$pkg| {
    #       package { $pkg['name']:
    #           ensure   => $pkg['version'] ? {
    #               ''      => 'latest',
    #               default => $pkg['version'],
    #           },
    #           provider => $pkg['source'],   # apt | opkg | pip
    #       }
    #   }
    #
    # See docs/tasks/DONE/02-puppet-provisioning-orchestration.md for
    # how this plugs together with the `artheia generate-manifest`
    # output once the .ipks land.

    notice("theia::provisioning: os_packages step is STUB (will read ${machine_yaml_path})")

    # ----- 2. Theia opkg artifacts under /opt/theia/ ----------------------
    #
    # Same shape: per `opkg_artifacts` entry, install the .ipk that was
    # built by `bazel build //platform/{supervisor,gateway}:ipk` and
    # shipped in the distribution tarball; drop the systemd unit; enable
    # + start the service.
    #
    #   $machine_yaml['machine']['opkg_artifacts'].each |$art| {
    #       package { "theia-${art['name']}":
    #           ensure   => 'installed',
    #           provider => 'dpkg',
    #           source   => "/var/theia/artifacts/${art['name']}.ipk",
    #       }
    #       file { $art['systemd_unit']:
    #           ensure  => 'file',
    #           source  => "puppet:///modules/theia/${art['name']}.service",
    #           notify  => Service["theia-${art['name']}"],
    #       }
    #       service { "theia-${art['name']}":
    #           ensure => 'running',
    #           enable => $art['enable_on_boot'],
    #       }
    #   }

    notice("theia::provisioning: opkg_artifacts step is STUB (supervisor + gateway)")

    # ----- 3. services/db schema-version check + offline migration --------
    #
    # Deliberately commented until services/db lands. The intended shape:
    #
    #   exec { 'theia-db-migrate-check':
    #       command => '/opt/theia/services-db/bin/migrate --check',
    #       returns => [0, 1],   # 0=no delta, 1=delta-present
    #   }
    #   exec { 'theia-db-migrate':
    #       command     => '/opt/theia/services-db/bin/migrate --apply',
    #       refreshonly => true,
    #       subscribe   => Exec['theia-db-migrate-check'],
    #       require     => Service['theia-supervisor'],
    #       before      => Service['theia-supervisor'],
    #   }
    #
    # The migrate binary must be idempotent and atomically swap the
    # schema-version key in etcd on success.

    notice("theia::provisioning: services/db migration step is DEFERRED")
}
