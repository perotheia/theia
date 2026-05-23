# theia::orchestration — Phase 2 of the two-phase deploy model.
#
# When this runs:
#   - Day-to-day application updates (FC daemons, demo binaries, …)
#   - Anything that does NOT touch supervisor / gateway / etcd / schema
#
# What it does:
#   1. For every entry in application.yaml's components list,
#      install / upgrade the corresponding /opt/theia/apps/<name>.ipk.
#   2. Notify the supervisor to reload its child table (no restart).
#
# Per-machine config source: /etc/theia/manifest/application.yaml
#   Same Distribution tarball as machine.yaml. Orchestration runs are
#   triggered remotely (push) when a new release of an application
#   is published; the operator pushes a fresh .ipk + an updated
#   application.yaml side-by-side, then runs `puppet apply
#   orchestration.pp`.
#
# Full updates (supervisor / gateway / etcd) DO NOT use this path —
# they go through theia::provisioning instead, which restarts the
# whole stack and runs any necessary services/db migration first.

class theia::orchestration (
    String $machine,
    String $manifest_dir = "/etc/theia/manifest",
    String $app_install_root = "/opt/theia/apps",
) {
    $application_yaml_path = "${manifest_dir}/application.yaml"

    file { $app_install_root:
        ensure => directory,
        mode   => '0755',
    }

    notice("theia::orchestration: reading ${application_yaml_path} for machine '${machine}'")

    # ----- 1. Replace application .ipks ----------------------------------
    #
    # Real implementation iterates application.yaml's components list
    # and reinstalls each .ipk if the on-disk version differs. The
    # supervisor doesn't restart — it watches /opt/theia/apps/ and
    # spawns the child binary as instructed by its in-process
    # SupervisorTree (the executor.yaml from the rig).
    #
    #   $app_yaml = loadyaml($application_yaml_path)
    #   $app_yaml['applications'].each |$app| {
    #       $app['components'].each |$cmp| {
    #           package { "theia-app-${cmp['name']}":
    #               ensure   => 'latest',
    #               provider => 'dpkg',
    #               source   => "/var/theia/apps/${cmp['name']}.ipk",
    #               notify   => Exec['theia-supervisor-reload'],
    #           }
    #       }
    #   }
    #
    # `ensure => latest` is intentional: the operator's push moves
    # /var/theia/apps/<name>.ipk to the new bits before applying
    # this manifest, and dpkg compares versions.

    notice("theia::orchestration: application .ipk install step is STUB")

    # ----- 2. Tell the supervisor to reload (no restart) ----------------
    #
    # The supervisor exposes a control plane (services/com gRPC bridge)
    # that accepts RestartChild / TerminateChild / StartChild. Touch
    # the reload endpoint so it re-reads the executor.yaml in case
    # processes were added / removed. This is the orchestration-side
    # equivalent of `systemctl daemon-reload` — no service restarts,
    # just a signal that the spec changed.
    #
    #   exec { 'theia-supervisor-reload':
    #       command     => '/opt/theia/supervisor/bin/supdbg reload',
    #       refreshonly => true,
    #   }
    #
    # If a child process needs to actually restart (e.g. new binary),
    # the operator follows up with a targeted `supdbg restart <name>`.
    # Orchestration does NOT do blanket restarts — that's a
    # provisioning concern.

    notice("theia::orchestration: supervisor reload step is STUB")
}
