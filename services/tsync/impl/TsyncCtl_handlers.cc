// User handler bodies for TsyncCtl — the Time-Sync control node.
//
// A THIN control plane over linuxptp / chrony: it POLLS the daemons (via
// impl/ptp_backend.hpp), caches the latest SyncStatus in State, serves
// GetSyncStatus, and logs a status line on a state edge (loss-of-lock → a health
// event for PHM). It NEVER disciplines the clock — the kernel + the PTP daemons
// do. Graceful-degrades to UNAVAILABLE/SYSTEM when no daemon is present (the dev
// host has no linuxptp). See docs/tasks/PROGRESS/AUTOSAR-LINUX-mapping.md.

#include "lib/TsyncCtl.hh"

#include "impl/ptp_backend.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include <pb_decode.h>

#include "TimerService.hh"   // post_info / send_after / process_timers

namespace ara::tsync {

namespace {
// One status poll → update State. Logs only on a STATE EDGE (so the log isn't
// spammed every tick); a downward edge from LOCKED is the loss-of-lock event a
// PHM would escalate on.
void poll_once(TsyncCtl& self, TsyncCtlState& s) {
    auto snap = PtpBackend::poll(s.ptp_interface, s.cfg_source, s.lock_offset_ns);
    s.state       = snap.state;
    s.source      = snap.source;
    s.offset_ns   = snap.offset_ns;
    s.grandmaster = snap.grandmaster;
    s.interface   = snap.interface.empty() ? s.ptp_interface : snap.interface;
    s.message     = snap.message;
    if (s.state != s.last_reported_state) {
        static const char* kSt[] = {"UNAVAILABLE", "UNLOCKED", "HOLDOVER", "LOCKED"};
        static const char* kSrc[] = {"SYSTEM", "PTP", "NTP"};
        char line[256];
        std::snprintf(line, sizeof(line),
                      "sync %s via %s (offset=%lldns gm=%s iface=%s) — %s",
                      kSt[s.state & 3], kSrc[s.source % 3],
                      s.offset_ns, s.grandmaster.empty() ? "-" : s.grandmaster.c_str(),
                      s.interface.empty() ? "-" : s.interface.c_str(),
                      s.message.c_str());
        if (s.state < s.last_reported_state)
            self.log().warn(std::string("LOSS OF LOCK: ") + line);
        else
            self.log().info(line);
        s.last_reported_state = s.state;
    }
}
}  // namespace

// init: schedule the first poll tick (requires_timers gives us send_after).
void TsyncCtl::init(TsyncCtlState& s) {
    log().info("tsync up — managing linuxptp/chrony (status-only; kernel "
               "disciplines the clock)");
    ::theia::runtime::post_info(*this, "poll");
}

// handle_info "poll": poll the backend, then reschedule at the config cadence.
void TsyncCtl::handle_info(const char* info, TsyncCtlState& s) {
    if (info && std::strcmp(info, "poll") == 0) {
        poll_once(*this, s);
        uint32_t ms = s.poll_ms ? s.poll_ms : 1000;
        ::theia::runtime::send_after(::theia::runtime::process_timers(), ms,
                                     *this, "poll");
    }
}

// on_config_update: apply the new TsyncConfig live (poll cadence, interface,
// source, lock gate). The next tick uses it; no restart.
void TsyncCtl::on_config_update(const platform_runtime_ConfigUpdated& push,
                                TsyncCtlState& s) {
    system_services_tsync_TsyncConfig cfg =
        system_services_tsync_TsyncConfig_init_zero;
    pb_istream_t is = pb_istream_from_buffer(push.config.bytes, push.config.size);
    if (!pb_decode(&is, system_services_tsync_TsyncConfig_fields, &cfg)) {
        log().warn("on_config_update: TsyncConfig decode failed — not applied");
        return;
    }
    s.ptp_interface  = cfg.ptp_interface;
    s.ptp_domain     = cfg.ptp_domain;
    s.cfg_source     = cfg.source[0] ? cfg.source : "ptp";
    s.poll_ms        = cfg.poll_ms ? cfg.poll_ms : 1000;
    s.lock_offset_ns = cfg.lock_offset_ns ? cfg.lock_offset_ns : 100000;
    log().info(std::string("tsync config applied: iface=") + s.ptp_interface +
               " source=" + s.cfg_source + " poll_ms=" + std::to_string(s.poll_ms));
}

// GetSyncStatus: serve the cached snapshot (sensor pipelines / tdb / PHM read it).
SyncStatus TsyncCtl::handle_call(const SyncStatusReq& /*req*/,
                                 TsyncCtlState& s) {
    SyncStatus rep = system_services_tsync_SyncStatus_init_zero;
    rep.state     = static_cast<system_services_tsync_SyncState>(s.state);
    rep.source    = static_cast<system_services_tsync_TimeSource>(s.source);
    rep.offset_ns = s.offset_ns;
    std::snprintf(rep.grandmaster, sizeof(rep.grandmaster), "%s", s.grandmaster.c_str());
    std::snprintf(rep.interface, sizeof(rep.interface), "%s", s.interface.c_str());
    std::snprintf(rep.message, sizeof(rep.message), "%s", s.message.c_str());
    rep.ts_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    return rep;
}

}  // namespace ara::tsync
