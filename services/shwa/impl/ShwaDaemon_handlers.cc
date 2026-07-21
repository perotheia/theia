// User handler bodies for ShwaDaemon — the Safe-Hardware-Accelerator node.
//
// HAND-OWNED (gen-fc emits this once, then skips it without --force).
//
// On a timer tick ShwaDaemon samples the build-selected backend (host or
// jetson), broadcasts an AccelTelemetry record (CPU/GPU/mem/temp/power/fan),
// reconciles the Orin power mode, and serves GetAccelStatus / SetPowerMode. The
// power plane (nvpmodel/jetson_clocks) lives HERE now — moved from osi, where it
// was misplaced. Read-only telemetry + the power knob; never a data path.

#include "lib/ShwaDaemon.hh"
#include "impl/shwa_backend.hpp"

#include "TimerService.hh"      // post_info / send_after / process_timers
#include "TheiaMsgHeader.hh"    // TheiaMsgHeader + kBusTypeRpc / kMsgGenCast
#include "RemoteCodec.hh"       // hash_msg_type_ (AccelSample service_id)
#include "MachineInstance.hh"   // resolve_node_tipc → this machine's instance

#include <pb_decode.h>
#include <pb_encode.h>

#include <linux/tipc.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace ara::shwa {

namespace {

uint64_t now_ns_() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

// This machine's TIPC instance (central=0, compute=1, …), set once from main
// via set_machine_index() and stamped on every AccelSample so com can demux the
// shared-namespace telemetry per machine. Read on the poll thread, written once
// before the daemon starts — relaxed atomic is enough.
std::atomic<uint32_t> g_machine_index{0};

// Fill an AccelSample wire message from a backend AccelReading.
AccelSample to_wire_(const AccelReading& r) {
    AccelSample m = system_services_shwa_AccelSample_init_zero;
    std::strncpy(m.board, r.board.c_str(), sizeof(m.board) - 1);
    m.on_jetson    = r.on_jetson;
    m.power_mode   = static_cast<system_services_shwa_PowerMode>(r.power_mode);
    m.cpu_util_pct = r.cpu_util_pct;
    m.cpu_count    = r.cpu_count;
    m.cpu_freq_mhz = r.cpu_freq_mhz;
    m.gpu_util_pct = r.gpu_util_pct;
    m.gpu_freq_mhz = r.gpu_freq_mhz;
    m.mem_used_mb  = r.mem_used_mb;
    m.mem_total_mb = r.mem_total_mb;
    m.temp_c       = r.temp_c;
    m.power_mw     = r.power_mw;
    m.fan_rpm      = r.fan_rpm;
    m.uptime_sec             = r.uptime_sec;
    m.disk_root_total_kb     = r.disk_root_total_kb;
    m.disk_root_avail_kb     = r.disk_root_avail_kb;
    m.disk_install_total_kb  = r.disk_install_total_kb;
    m.disk_install_avail_kb  = r.disk_install_avail_kb;
    m.ts_ns        = now_ns_();
    // Cluster identity. machine_name is left empty here — com fills it from the
    // machine manifest (it has the index→name map); shwa only knows its index.
    m.machine_index = g_machine_index.load(std::memory_order_relaxed);
    return m;
}

// ── AccelTelemetry egress: now PG (SHWA → com) ───────────────────────────────
//
// The hand-rolled AccelSubmitter (a SOCK_DGRAM sendto to a fixed egress name
// 0x8001001A, the AccelSample analogue of the old TraceSubmitter) is RETIRED.
// shwa's generated broadcast_broadcast_sample() is now a PG name-sequence
// multicast of AccelSample; com's shwa_link pg_joins the AccelSample group, so
// the supervisor allocates com's delivery address and the kernel fans out the
// sample. No fixed egress address, no per-process bind collision, no submitter.

}  // namespace

// Cluster identity setter — see the declaration in ShwaDaemon.hh. Stamped on
// every AccelSample by to_wire_() so com can demux per-machine telemetry.
void set_machine_index(uint32_t machine_index) noexcept {
    g_machine_index.store(machine_index, std::memory_order_relaxed);
}

// init: bring up the backend, apply config, kick off the poll loop.
void ShwaDaemon::init(ShwaDaemonState& s) {
    backend::init();
    s.started = true;
    s.power_mode = backend::read_power_mode();   // initial read (UNKNOWN off-Jetson)
    backend::sample(s.last);                     // a first reading for GetAccelStatus

    // Cluster identity for telemetry. main.cc already parsed --tipc; resolve our
    // own node's instance (central=0, compute=1, …) and stamp it on every
    // AccelSample so com can demux the shared-namespace egress per machine.
    uint32_t my_type = 0, my_inst = 0;
    ::theia::runtime::resolve_node_tipc(kNodeName, kTipcType, kTipcInstance,
                                        my_type, my_inst);
    set_machine_index(my_inst);

    log().info(std::string("shwa up — hardware telemetry + power "
        "(board=") + s.last.board + ", on_jetson=" +
        (backend::on_jetson() ? "yes" : "no") +
        ", machine=" + std::to_string(my_inst) + ")");
    ::theia::runtime::post_info(*this, "poll");
}

// handle_info "poll": sample, broadcast, reconcile power, reschedule.
void ShwaDaemon::handle_info(const char* info, ShwaDaemonState& s) {
    if (!info || std::strcmp(info, "poll") != 0) return;

    backend::sample(s.last);
    AccelSample wire = to_wire_(s.last);
    broadcast_broadcast_sample(wire);          // PG multicast → com (GUI bridge) + any joiner

    // Reconcile the desired power mode on Orin (edge — only on a change).
    if (backend::on_jetson() && s.power_mode != s.applied_power_mode &&
        s.power_mode != PM_UNKNOWN) {
        std::string err;
        if (backend::apply_power_mode(s.power_mode, s.jetson_clocks, s.persist, err)) {
            s.applied_power_mode = s.power_mode;
            log().info("power mode → " + std::to_string(s.power_mode) +
                       (s.jetson_clocks ? " (+jetson_clocks)" : "") +
                       (s.persist ? " (persisted)" : ""));
        } else {
            // apply_power_mode returns false on a persist-only failure too (the
            // live switch happened, the reboot default didn't) — log it either way.
            log().warn("power mode apply: " + err);
        }
    }

    uint32_t ms = s.poll_ms ? s.poll_ms : 2000;
    ::theia::runtime::send_after(::theia::runtime::process_timers(), ms,
                                 *this, "poll");
}

// on_config_update: apply ShwaConfig live (cadence, desired power mode).
void ShwaDaemon::on_config_update(const platform_runtime_ConfigUpdated& cfg,
                                  ShwaDaemonState& s) {
    system_services_shwa_ShwaConfig c = system_services_shwa_ShwaConfig_init_zero;
    pb_istream_t is = pb_istream_from_buffer(
        reinterpret_cast<const pb_byte_t*>(cfg.config.bytes), cfg.config.size);
    if (!pb_decode(&is, system_services_shwa_ShwaConfig_fields, &c)) {
        log().warn("on_config_update: ShwaConfig decode failed — not applied");
        return;
    }
    s.poll_ms       = c.poll_ms ? c.poll_ms : 2000;
    s.power_mode    = static_cast<int>(c.power_mode);
    s.jetson_clocks = c.jetson_clocks;
    s.persist       = c.persist;
    // Re-arm the reconcile edge so a config change to persist/power_mode is
    // re-applied on the next tick even if the mode value itself is unchanged
    // (persist alone flipping true must still write nvpmodel.conf's DEFAULT).
    s.applied_power_mode = -1;
    log().info(std::string("config: poll_ms=") + std::to_string(s.poll_ms) +
        " power_mode=" + std::to_string(s.power_mode) +
        " jetson_clocks=" + (s.jetson_clocks ? "true" : "false") +
        " persist=" + (s.persist ? "true" : "false"));
}

// GetAccelStatus — serve the latest sample.
AccelSample ShwaDaemon::handle_call(const AccelStatusReq& /*req*/,
                                    ShwaDaemonState& s) {
    return to_wire_(s.last);
}

// SetPowerMode — set the Orin power profile (no-op + applied=false off-Jetson).
PowerModeReply ShwaDaemon::handle_call(const PowerModeReq& req,
                                       ShwaDaemonState& s) {
    PowerModeReply rep = system_services_shwa_PowerModeReply_init_zero;
    std::string err;
    if (!backend::on_jetson()) {
        rep.applied = false;
        std::strncpy(rep.message, "not a Jetson — power mode unmanaged",
                     sizeof(rep.message) - 1);
        return rep;
    }
    // The ad-hoc SetPowerMode op is TRANSIENT (current boot only) — PowerModeReq
    // has no persist field; persistence is a config concern (ShwaConfig.persist).
    bool ok = backend::apply_power_mode(static_cast<int>(req.mode),
                                        req.jetson_clocks, /*persist=*/false, err);
    if (ok) {
        s.power_mode = static_cast<int>(req.mode);
        s.applied_power_mode = s.power_mode;
        s.jetson_clocks = req.jetson_clocks;
        std::strncpy(rep.message, "applied", sizeof(rep.message) - 1);
    } else {
        std::strncpy(rep.message, err.c_str(), sizeof(rep.message) - 1);
    }
    rep.applied = ok;
    return rep;
}

}  // namespace ara::shwa
