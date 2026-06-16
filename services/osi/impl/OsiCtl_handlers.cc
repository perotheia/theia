// User handler bodies for OsiCtl — the OS-resource + power control node.
//
// HAND-OWNED (gen-app emits this once, then skips it without --force).
//
// OsiCtl owns the two resource-plane pieces the runtime+supervisor don't:
// cgroup v2 per-FC partitioning/accounting + Orin power-mode reconciliation. On
// a timer tick it (a) discovers the FC processes (children of the supervisor),
// (b) reads their /proc CPU%/RSS + cgroup limits, (c) caches + broadcasts
// ResourceStatus, (d) reconciles the desired power mode on Orin. SetResourceLimit
// writes a child's cgroup v2 cpu.max / memory.high. It NEVER forks a child — the
// supervisor (EM) owns lifecycle; OSI owns the slices.
//
// All of it graceful-degrades: no cgroup delegation → limits report "unlimited"
// and writes fail cleanly; no Jetson tooling → on_jetson=false / MODE_UNKNOWN.

#include "lib/OsiCtl.hh"
#include "impl/osi_backend.hpp"

#include "TimerService.hh"   // post_info / send_after / process_timers

#include <pb_decode.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace ara::osi {

namespace {

uint64_t now_ns_() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

// The FC short names OSI manages (= the supervisor's service children = the
// cgroup sub-dirs). Kept in sync with services/manifest/service.py SERVICES_SHORTS
// + the demo apps the supervisor forks. Empty-match accepts any supervisor
// child, so this is a label filter, not a hard gate.
const std::vector<std::string> kKnownFcs = {
    "com", "crypto", "log", "nm", "osi", "per", "sm", "tsync", "ucm", "shwa",
    "p1", "p2", "p3", "p4",
};

}  // namespace

// init: discover the supervisor (our parent), probe the power plane once, and
// kick off the poll loop. requires_timers gives us send_after.
void OsiCtl::init(OsiCtlState& s) {
    s.supervisor_pid = ::getppid();
    s.on_jetson = on_jetson();
    s.power_mode = read_power_mode();   // initial read (UNKNOWN off-Jetson)
    // Create the Theia slice + delegate cpu/memory controllers. Best-effort:
    // succeeds when OSI runs as a root child of the root supervisor (the real
    // stack); on an unprivileged dev-host run it fails cleanly and only the
    // limit-WRITING path is unavailable — /proc accounting still works.
    s.slice_ready = ensure_slice(s.cgroup_root, s.slice_name);
    log().info(std::string("osi up — cgroup v2 resource plane + power mode "
        "(supervisor pid=") + std::to_string(s.supervisor_pid) +
        ", slice=" + (s.slice_ready ? "ready" : "unavailable") +
        ", on_jetson=" + (s.on_jetson ? "yes" : "no") + ")");
    ::theia::runtime::post_info(*this, "poll");
}

// handle_info "poll": sample every FC, compute CPU% via jiffies-delta, broadcast
// the snapshot, reconcile power mode, then reschedule at the config cadence.
void OsiCtl::handle_info(const char* info, OsiCtlState& s) {
    if (!info || std::strcmp(info, "poll") != 0) return;

    const uint64_t now = now_ns_();
    const uint64_t dwall_ms = (s.last_sample_ns && now > s.last_sample_ns)
        ? (now - s.last_sample_ns) / 1000000ull : 0;

    auto samples = discover_fcs(s.supervisor_pid, kKnownFcs);

    system_services_osi_ResourceStatus snap =
        system_services_osi_ResourceStatus_init_zero;
    snap.power_mode = static_cast<system_services_osi_PowerMode>(s.power_mode);
    snap.on_jetson  = s.on_jetson;
    snap.ts_ns      = now;

    std::map<std::string, uint64_t> fresh_jiffies;
    pb_size_t n = 0;
    for (const auto& ps : samples) {
        if (n >= 32) break;   // wire cap (max_count:32)
        fresh_jiffies[ps.fc] = ps.cpu_jiffies;

        system_services_osi_FcResource& r = snap.fcs[n++];
        r = system_services_osi_FcResource_init_zero;
        std::strncpy(r.fc, ps.fc.c_str(), sizeof(r.fc) - 1);
        r.pid = static_cast<uint32_t>(ps.pid);
        r.rss_bytes = ps.rss_bytes;

        // CPU% from the jiffies delta vs the last poll (0 on the first tick).
        auto it = s.last_jiffies.find(ps.fc);
        if (it != s.last_jiffies.end() && dwall_ms &&
            ps.cpu_jiffies >= it->second) {
            r.cpu_pct = cpu_pct_from_delta(ps.cpu_jiffies - it->second, dwall_ms);
        }
        // Place the FC into its slice sub-cgroup (idempotent) so cpu.max /
        // memory.high written by SetResourceLimit bind to it. Only when the
        // slice is delegated (root child of the root supervisor).
        std::string cg = fc_cgroup_dir(s.cgroup_root, s.slice_name, ps.fc);
        if (s.slice_ready) place_pid(cg, ps.pid);
        // Echo the applied cgroup limits (read live; fall back to our cache).
        r.cpu_max_pct = read_cpu_max_pct(cg);
        r.mem_high    = read_mem_high(cg);
        if (r.cpu_max_pct == 0 && r.mem_high == 0) {
            auto lit = s.limits.find(ps.fc);
            if (lit != s.limits.end()) {
                r.cpu_max_pct = lit->second.cpu_max_pct;
                r.mem_high    = lit->second.mem_high;
            }
        }
    }
    snap.fcs_count = n;
    snap.fc_count  = n;

    s.last_jiffies = std::move(fresh_jiffies);
    s.last_sample_ns = now;
    s.last_snapshot = snap;

    broadcast_broadcast_status(snap);

    // Reconcile power mode on Orin: push the desired mode on an edge.
    if (s.on_jetson && s.power_mode != s.applied_power_mode &&
        s.power_mode != P_UNKNOWN) {
        std::string err;
        if (apply_power_mode(s.power_mode, s.jetson_clocks, err)) {
            s.applied_power_mode = s.power_mode;
            log().info("power mode → " + std::to_string(s.power_mode) +
                       (s.jetson_clocks ? " (+jetson_clocks)" : ""));
        } else {
            log().warn("power mode apply failed: " + err);
        }
    }

    uint32_t ms = s.poll_ms ? s.poll_ms : 2000;
    ::theia::runtime::send_after(::theia::runtime::process_timers(), ms,
                                 *this, "poll");
}

// on_config_update: apply OsiConfig live (cgroup root/slice, poll cadence,
// desired power mode, jetson_clocks). The next tick uses it; no restart.
void OsiCtl::on_config_update(
        const platform_runtime_ConfigUpdated& cfg,
        OsiCtlState& s) {
    system_services_osi_OsiConfig c = system_services_osi_OsiConfig_init_zero;
    pb_istream_t is = pb_istream_from_buffer(
        reinterpret_cast<const pb_byte_t*>(cfg.config.bytes), cfg.config.size);
    if (!pb_decode(&is, system_services_osi_OsiConfig_fields, &c)) {
        log().warn("on_config_update: OsiConfig decode failed — not applied");
        return;
    }
    if (c.cgroup_root[0]) s.cgroup_root = c.cgroup_root;
    if (c.slice_name[0])  s.slice_name  = c.slice_name;
    s.poll_ms       = c.poll_ms ? c.poll_ms : 2000;
    s.power_mode    = static_cast<int>(c.power_mode);
    s.jetson_clocks = c.jetson_clocks;
    log().info(std::string("config: slice=") + s.cgroup_root + "/" + s.slice_name +
        " poll_ms=" + std::to_string(s.poll_ms) +
        " power_mode=" + std::to_string(s.power_mode) +
        " jetson_clocks=" + (s.jetson_clocks ? "true" : "false"));
}

// GetResourceStatus — serve the cached snapshot (refreshed each tick).
ResourceStatus OsiCtl::handle_call(
        const ResourceStatusReq& /*req*/,
        OsiCtlState& s) {
    return s.last_snapshot;
}

// SetResourceLimit — write the FC's cgroup v2 cpu.max / memory.high and cache
// the intent (echoed in the next snapshot). Reports applied=false + a reason
// where the cgroup isn't writable (no v2 delegation on this host).
ResourceLimitReply OsiCtl::handle_call(
        const ResourceLimitReq& req,
        OsiCtlState& s) {
    ResourceLimitReply rep = system_services_osi_ResourceLimitReply_init_zero;
    std::string fc = req.fc;
    if (fc.empty()) {
        rep.applied = false;
        std::strncpy(rep.message, "empty fc name", sizeof(rep.message) - 1);
        return rep;
    }
    if (!s.slice_ready) {
        rep.applied = false;
        std::strncpy(rep.message,
            "cgroup slice unavailable (not delegated / unprivileged host)",
            sizeof(rep.message) - 1);
        return rep;
    }
    std::string cg = fc_cgroup_dir(s.cgroup_root, s.slice_name, fc);
    std::string err;
    // The FC may not have been placed yet (limit set before its first poll) —
    // ensure the sub-cgroup exists so the cpu.max/memory.high writes land.
    ::mkdir(cg.c_str(), 0755);
    bool ok = apply_limit(cg, req.cpu_max_pct, req.mem_high, err);
    if (ok) {
        s.limits[fc] = {req.cpu_max_pct, req.mem_high};
        std::string m = "applied cpu_max=" + std::to_string(req.cpu_max_pct) +
            "% mem_high=" + std::to_string(req.mem_high) + "B to " + fc;
        std::strncpy(rep.message, m.c_str(), sizeof(rep.message) - 1);
        log().info(m);
    } else {
        std::strncpy(rep.message, err.c_str(), sizeof(rep.message) - 1);
        log().warn("SetResourceLimit(" + fc + "): " + err);
    }
    rep.applied = ok;
    return rep;
}

}  // namespace ara::osi
