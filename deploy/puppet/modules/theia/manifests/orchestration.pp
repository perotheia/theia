# theia::orchestration — Phase 2 of the two-phase deploy model.
#
# When this runs:
#   - Day-to-day application updates (FC daemons, demo binaries, …)
#   - Anything that does NOT touch supervisor / gateway / schema
#
# What it does:
#   1. For every bazel_buildable component in application.json, install /
#      upgrade /opt/theia/apps/<name>.ipk.
#   2. Notify the supervisor to reload its child table (no restart) via tdb.
#
# Per-machine config source: ${manifest_dir}/application.json — same
#   Distribution tarball as machine.json. Orchestration runs are triggered
#   remotely (push) when a new application release is published: the operator
#   pushes a fresh .ipk + an updated application.json side-by-side, then runs
#   `puppet apply orchestration.pp`.
#
# Full updates (supervisor / gateway) DO NOT use this path — they go through
# theia::provisioning, which restarts the stack and runs any per migration.

class theia::orchestration (
    String $machine,
    String $manifest_dir     = "/etc/theia/manifest",
    String $app_install_root = "/opt/theia/apps",
    String $tdb_bin          = "/opt/theia/bin/tdb",
) {
    $application_json_path = "${manifest_dir}/application.json"

    file { $app_install_root:
        ensure => directory,
        mode   => '0755',
    }

    $manifest = parsejson(file($application_json_path))

    notice("theia::orchestration: ${application_json_path} → machine '${machine}'")

    # ----- 1. Install / upgrade application .ipks -------------------------
    #
    # Flatten applications[].components[] to the buildable ones; each has a
    # /opt/theia/apps/<name>.ipk staged beside the manifest. ensure => latest:
    # the operator's push moves the .ipk to the new bits before this runs and
    # dpkg compares versions. A version bump notifies the supervisor reload.
    $components = $manifest['applications'].reduce([]) |$acc, $app| {
        $acc + $app['components'].filter |$c| { $c['bazel_buildable'] }
    }

    $components.each |$cmp| {
        package { "theia-app-${cmp['name']}":
            ensure   => 'latest',
            provider => 'dpkg',
            source   => "${app_install_root}/${cmp['name']}.ipk",
            require  => File[$app_install_root],
            notify   => Exec['theia-supervisor-reload'],
        }
    }

    # ----- 2. Tell the supervisor to reload (no restart) ------------------
    #
    # The supervisor exposes a control plane (services/com gRPC bridge ↔ the
    # Theia transport) that tdb drives. A reload re-reads the executor tree so
    # added/removed children are picked up. refreshonly: only fires on a .ipk
    # change above. No blanket restarts — that's a provisioning concern.
    exec { 'theia-supervisor-reload':
        command     => "${tdb_bin} reload",
        refreshonly => true,
        path        => ['/usr/bin', '/bin', '/opt/theia/bin'],
    }
}
