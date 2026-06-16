// User handler bodies for FwDaemon — the firewall control node.
//
// HAND-OWNED (gen-app emits this once, then skips it without --force).
//
// FwDaemon generates a baseline nftables ruleset for the IP boundary (default-
// drop input, allow-list the DMZ TCP ports + loopback + established/related),
// merges config/fw.d/*.nft overrides, and applies it via `nft -f`. It NEVER
// sits in the data path — nft enforces. The inter-FC TIPC mesh is out of scope
// (AF_TIPC isn't IP-filterable); this governs com's gRPC DMZ + etcd.
//
// All of it graceful-degrades: no `nft` / no privilege → FW_DEGRADED + the
// diagnostic, never a crash. enabled=false → flush our table + FW_DISABLED.

#include "lib/FwDaemon.hh"
#include "impl/fw_backend.hpp"

#include "TimerService.hh"   // post_info / send_after / process_timers

#include <pb_decode.h>

#include <chrono>
#include <cstring>
#include <string>

namespace ara::fw {

namespace {

uint64_t now_ns_() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

// Regenerate + (re)apply the ruleset from the current config, fold the result
// into state, broadcast it. Shared by init / on_config_update / ReloadRules /
// the reassert tick.
void apply_now(FwDaemon& self, FwDaemonState& s) {
    if (!s.enabled) {
        flush_ruleset();
        s.state = F_DISABLED;
        s.rule_count = 0;
        s.override_count = 0;
        s.message = "firewall disabled (config) — table flushed";
        self.log().info(s.message);
    } else {
        int rules = 0, overrides = 0;
        std::string ruleset = build_ruleset(s.dmz_tcp_ports, s.default_policy,
                                            s.fw_d_dir, rules, overrides);
        ApplyResult r = apply_ruleset(ruleset, rules, overrides);
        s.rule_count = static_cast<uint32_t>(r.rule_count);
        s.override_count = static_cast<uint32_t>(r.override_count);
        s.message = r.message;
        s.state = r.ok ? F_APPLIED : F_DEGRADED;
        if (r.ok) self.log().info("nftables: " + r.message);
        else      self.log().warn("nftables: " + r.message);
    }

    // Broadcast the new status (→ firehose / PHM: an apply failure is a health
    // event).
    FwStatusMsg msg = system_services_fw_FwStatusMsg_init_zero;
    msg.state = static_cast<system_services_fw_FwState>(s.state);
    msg.rule_count = s.rule_count;
    msg.override_count = s.override_count;
    std::strncpy(msg.message, s.message.c_str(), sizeof(msg.message) - 1);
    msg.ts_ns = now_ns_();
    self.broadcast_broadcast_status(msg);
}

}  // namespace

// init: generate + apply the baseline ruleset once at boot; arm the reassert
// tick if configured (so a foreign nft flush self-heals).
void FwDaemon::init(FwDaemonState& s) {
    log().info("fw up — generating nftables baseline (DMZ allow-list + "
               "config/fw.d); the kernel enforces");
    apply_now(*this, s);
    if (s.reassert_ms) ::theia::runtime::post_info(*this, "reassert");
}

// handle_info "reassert": re-apply on a tick so the ruleset self-heals if
// another tool flushed/clobbered nft. Only armed when reassert_ms > 0.
void FwDaemon::handle_info(const char* info, FwDaemonState& s) {
    if (!info || std::strcmp(info, "reassert") != 0) return;
    apply_now(*this, s);
    if (s.reassert_ms)
        ::theia::runtime::send_after(::theia::runtime::process_timers(),
                                     s.reassert_ms, *this, "reassert");
}

// on_config_update: apply the new FwConfig live (re-generate + re-apply, or
// flush if disabled). The next tick uses the new cadence.
void FwDaemon::on_config_update(
        const platform_runtime_ConfigUpdated& cfg,
        FwDaemonState& s) {
    system_services_fw_FwConfig c = system_services_fw_FwConfig_init_zero;
    pb_istream_t is = pb_istream_from_buffer(
        reinterpret_cast<const pb_byte_t*>(cfg.config.bytes), cfg.config.size);
    if (!pb_decode(&is, system_services_fw_FwConfig_fields, &c)) {
        log().warn("on_config_update: FwConfig decode failed — not applied");
        return;
    }
    const bool was_ticking = s.reassert_ms != 0;
    s.enabled        = c.enabled;
    if (c.fw_d_dir[0])       s.fw_d_dir       = c.fw_d_dir;
    if (c.dmz_tcp_ports[0])  s.dmz_tcp_ports  = c.dmz_tcp_ports;
    if (c.default_policy[0]) s.default_policy = c.default_policy;
    s.reassert_ms    = c.reassert_ms;
    log().info(std::string("config: enabled=") + (s.enabled ? "1" : "0") +
        " policy=" + s.default_policy + " dmz=" + s.dmz_tcp_ports +
        " fw.d=" + s.fw_d_dir + " reassert_ms=" + std::to_string(s.reassert_ms));
    apply_now(*this, s);
    // If reassert was off and is now on, kick the tick (init's may have ended).
    if (!was_ticking && s.reassert_ms)
        ::theia::runtime::post_info(*this, "reassert");
}

// GetFirewallStatus — serve the cached apply result.
FwStatusMsg FwDaemon::handle_call(
        const FwStatusReq& /*req*/,
        FwDaemonState& s) {
    FwStatusMsg msg = system_services_fw_FwStatusMsg_init_zero;
    msg.state = static_cast<system_services_fw_FwState>(s.state);
    msg.rule_count = s.rule_count;
    msg.override_count = s.override_count;
    std::strncpy(msg.message, s.message.c_str(), sizeof(msg.message) - 1);
    msg.ts_ns = now_ns_();
    return msg;
}

// ReloadRules — force a regenerate + re-apply now (e.g. after editing
// config/fw.d/ on disk).
ReloadReply FwDaemon::handle_call(
        const ReloadReq& /*req*/,
        FwDaemonState& s) {
    apply_now(*this, s);
    ReloadReply rep = system_services_fw_ReloadReply_init_zero;
    rep.applied = (s.state == F_APPLIED || s.state == F_DISABLED);
    std::strncpy(rep.message, s.message.c_str(), sizeof(rep.message) - 1);
    return rep;
}

}  // namespace ara::fw
