# theia::postinstall — file capabilities (setcap) for the Theia binaries.
#
# The SINGLE SOURCE OF TRUTH for which Linux capabilities each binary needs to
# run UNPRIVILEGED (no root) yet do its one privileged thing. Included by BOTH
# the remote deploy path (theia::provisioning, root = /opt/theia) AND the local
# dev path (theia::local_install, root = the workspace install/<machine>/), so
# the cap contract is defined once and applied identically.
#
#   - supervisor: cap_sys_nice — set realtime scheduling (SCHED_FIFO/RR) + CPU
#     affinity on the FC node threads it pushes via THEIA_NODE_CFG, and raise
#     CAP_SYS_NICE into the ambient set so its forked children inherit it.
#   - gateway: cap_net_raw + cap_net_admin — open AF_PACKET / raw CAN+UDP
#     sockets + tweak link state for the ASAM-CMP bus capture.
#   - bin/fw: cap_net_admin — load the generated nftables ruleset (the FwDaemon
#     FC runs unprivileged but needs NET_ADMIN to `nft -f` the IP-DMZ firewall).
#     Without it the FC graceful-degrades to FW_DEGRADED (ruleset generated, not
#     installed) — so the cap is what flips it to FW_APPLIED.
#
# +eip = Effective + Inheritable + Permitted (active on exec, inheritable by
# children). Idempotent: re-applied every run; a binary copy/upgrade clears caps
# (so this MUST run AFTER the install/copy step — callers order it that way).
#
# Parameters:
#   root   — directory holding `supervisor` (and optionally `gateway`).
#            /opt/theia for a real deploy; install/<machine> for local dev.
#   caps   — binary basename => capability spec. Override only for unusual
#            layouts; the default is the canonical contract.

class theia::postinstall (
    String $root = '/opt/theia',
    Hash   $caps = {
        'supervisor' => 'cap_sys_nice',
        'gateway'    => 'cap_net_raw,cap_net_admin',
        'bin/fw'     => 'cap_net_admin',
    },
) {
    $caps.each |$bin, $capspec| {
        $path = "${root}/${bin}"
        # Only when the binary exists (gateway isn't in every stage), and only
        # when the cap isn't already set (getcap idempotency guard).
        exec { "theia-setcap-${bin}":
            command => "/usr/sbin/setcap ${capspec}+eip ${path}",
            onlyif  => "/usr/bin/test -x ${path}",
            unless  => "/bin/sh -c '/usr/sbin/getcap ${path} | /bin/grep -q .'",
            path    => ['/usr/sbin', '/usr/bin', '/bin'],
        }
    }
}
