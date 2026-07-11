// User handler bodies for TsyncCtl — the Time-Sync control node.
//
// tsync owns TIME, nothing else (the tsync/location inversion): external time
// sources REGISTER at startup (handle_call RegisterTimeSourceReq — idempotent
// upsert) and CAST TimeObservation records (handle_cast). On each tick it
// SELECTS the best live registered source (highest priority among fresh ones —
// fresh = within 3× the source's advertised interval), falls back to the ptp4l
// backend (impl/ptp_backend.hpp) then the system wall clock, and caches the
// pick as the latest SyncStatus (served by GetSyncStatus). A state edge logs a
// status line + a health event for PHM (loss-of-lock). On central
// (config.discipline_clock=true) it STEPS CLOCK_REALTIME from the selected
// observation (clock_settime); phc2sys + ptp4l then distribute it. EPERM (no
// CAP_SYS_TIME) on the clock step is logged once and tolerated. The source's
// POSITION output is none of tsync's business — that registers with the
// location framework (theia_pkgs/location). See
// docs/tasks/PROGRESS/tsync-location-inversion.md.

#include "lib/TsyncCtl.hh"

#include "impl/ptp_backend.hpp"

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include <pb_decode.h>

#include "TimerService.hh"   // post_info / send_after / process_timers
#include "ParamsConfig.hh"   // get_config() — poll_ms

namespace ara::tsync {

namespace {
uint64_t wall_now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}
}  // namespace

namespace {

// Step CLOCK_REALTIME to `utc_ns` (gated + best-effort). Only steps when
// discipline_clock is set AND the offset exceeds the step threshold (a large
// initial correction; we don't chase small jitter — that's phc2sys/PPS work in a
// later rev). EPERM (no CAP_SYS_TIME on the dev host) is logged ONCE and
// tolerated — the FC keeps reporting the source's time without owning the clock.
constexpr long long kStepThresholdNs = 100 * 1000000LL;   // 100 ms

void maybe_discipline(TsyncCtl& self, TsyncCtlState& s, uint64_t src_utc_ns) {
    if (!s.discipline_clock) return;
    long long off = (long long)src_utc_ns - (long long)wall_now_ns();
    long long mag = off < 0 ? -off : off;
    if (mag < kStepThresholdNs && s.ext_disciplined) return;  // already close
    struct timespec ts;
    ts.tv_sec  = (time_t)(src_utc_ns / 1000000000ULL);
    ts.tv_nsec = (long)(src_utc_ns % 1000000000ULL);
    if (::clock_settime(CLOCK_REALTIME, &ts) == 0) {
        s.ext_disciplined = true;
        self.log().info(std::string("disciplined CLOCK_REALTIME from time source (stepped ") +
                        std::to_string(off) + "ns)");
    } else if (errno == EPERM) {
        if (!s.settime_eperm_logged) {
            self.log().warn("clock_settime: EPERM (no CAP_SYS_TIME) — reporting source "
                            "time but NOT stepping the host clock");
            s.settime_eperm_logged = true;
        }
    } else {
        self.log().warn(std::string("clock_settime failed: ") + std::strerror(errno));
    }
}

// Pick the best LIVE registered source: highest priority among the FRESH ones
// (an observation within 3× the source's advertised interval). Returns
// s.sources.end() when none is fresh. Ties break by name (map order) so the
// pick is deterministic.
std::map<std::string, RegisteredTimeSource>::const_iterator
select_source(const TsyncCtlState& s, uint64_t now) {
    auto best = s.sources.end();
    for (auto it = s.sources.begin(); it != s.sources.end(); ++it) {
        const auto& src = it->second;
        if (!src.last_obs_wall_ns || !src.utc_ns) continue;   // never observed
        const uint64_t fresh_ns =
            (uint64_t)src.expected_interval_ms * 3ULL * 1000000ULL;
        if (now - src.last_obs_wall_ns > fresh_ns) continue;   // stale
        if (best == s.sources.end() || src.priority > best->second.priority)
            best = it;
    }
    return best;
}

// One status tick → update State. SELECT from the registered sources first (the
// acquisition plane on central); if none is live, fall through to the PTP
// backend (the slave lock on compute), then the wall clock. Logs only on a
// STATE EDGE; a downward edge from LOCKED is the loss-of-lock event PHM
// escalates on.
void poll_once(TsyncCtl& self, TsyncCtlState& s) {
    const uint64_t now = wall_now_ns();

    const auto best = select_source(s, now);
    if (best != s.sources.end()) {
        const auto& src = best->second;
        // Project the observation's UTC to "now": the (utc_ns, local_ns) pair
        // removes the cast/queue latency between capture and this tick.
        const uint64_t utc_now = src.utc_ns + (now - src.local_ns);
        s.last_obs_ns = now;
        maybe_discipline(self, s, utc_now);
        s.source      = T_EXTERNAL;
        s.state       = S_LOCKED;
        s.offset_ns   = (long long)utc_now - (long long)now;
        s.grandmaster = "EXT(" + best->first + "/" + src.kind + ")" +
                        (src.pps_aligned ? " PPS" : "");
        s.interface   = "";
        s.message     = "";
    } else if (s.last_obs_ns && now - s.last_obs_ns <
               (uint64_t)s.source_timeout_ms * 1000000ULL) {
        // A recently-live source went silent — hold EXTERNAL HOLDOVER
        // (free-run) until source_timeout_ms, then drop to PTP/system.
        s.source  = T_EXTERNAL;
        s.state   = S_HOLDOVER;
        s.message = "registered sources silent — holdover";
    } else {
        // No live registered source — report the PTP lock (compute slave) or
        // degrade to the wall clock.
        auto snap = PtpBackend::poll(s.ptp_interface, s.cfg_source, s.lock_offset_ns);
        s.state       = snap.state;
        s.source      = snap.source;
        s.offset_ns   = snap.offset_ns;
        s.grandmaster = snap.grandmaster;
        s.interface   = snap.interface.empty() ? s.ptp_interface : snap.interface;
        s.message     = snap.message;
    }

    if (s.state != s.last_reported_state) {
        static const char* kSt[] = {"UNAVAILABLE", "UNLOCKED", "HOLDOVER", "LOCKED"};
        static const char* kSrc[] = {"SYSTEM", "PTP", "EXTERNAL"};
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

        // PHM health edge (escalation model): report the clock-health INDICATION
        // to PHM on this state EDGE only (the supervisor's heartbeat owns liveness;
        // tsync must not firehose "still LOCKED"). tsync does its own fault
        // analysis — a degraded clock is a platform-wide fault PHM aggregates:
        //   LOCKED(3)            → OK       (lock acquired / fault cleared)
        //   HOLDOVER(2)          → WARNING  (free-running, still usable — recoverable)
        //   UNLOCKED/UNAVAIL(≤1) → DEGRADED (no usable time discipline)
        // PHM aggregates → PhmHealthStatus → SM. CAST over PG.
        {
            FcHealthReport hr = system_services_phm_FcHealthReport_init_zero;
            std::snprintf(hr.entity, sizeof(hr.entity), "%s", TsyncCtl::kNodeName);
            hr.fg    = 2;          // FG_NETWORK (sm sm_sup_link FgId — tsync ∈ network_sup)
            hr.ts_ns = now;
            if (s.state == S_LOCKED) {
                hr.level = system_services_phm_HealthLevel_HealthLevel_OK;
                hr.code  = 0;
                std::snprintf(hr.detail, sizeof(hr.detail), "clock locked");
            } else if (s.state == S_HOLDOVER) {
                hr.level = system_services_phm_HealthLevel_HealthLevel_WARNING;
                hr.code  = 1;
                std::snprintf(hr.detail, sizeof(hr.detail), "clock holdover (free-run)");
            } else {
                hr.level = system_services_phm_HealthLevel_HealthLevel_DEGRADED;
                hr.code  = 2;
                std::snprintf(hr.detail, sizeof(hr.detail),
                              "no time lock (state=%s)", kSt[s.state & 3]);
            }
            self.broadcast_to_phm_report(hr);
        }

        s.last_reported_state = s.state;
    }
}
}  // namespace

// init: read the static deploy params (config/tsync.json), then schedule the
// first selection tick (requires_timers gives us send_after). No source is
// compiled in or assumed — the table starts empty; providers register when
// (and if) they come up.
void TsyncCtl::init(TsyncCtlState& s) {
    auto cfg = ::theia::runtime::get_config().node(TsyncCtl::kNodeName);
    s.poll_ms = cfg.u32("poll_ms", s.poll_ms);        // 100 (10 Hz) default
    log().info(std::string("tsync up — pure time plane; sources register at "
               "runtime (none yet), linuxptp distribution/fallback, poll_ms=") +
               std::to_string(s.poll_ms) + " discipline_clock=" +
               (s.discipline_clock ? "true" : "false"));
    ::theia::runtime::post_info(*this, "poll");
}

// handle_info "poll": run the selection tick, then reschedule at the cadence.
void TsyncCtl::handle_info(const char* info, TsyncCtlState& s) {
    if (info && std::strcmp(info, "poll") == 0) {
        poll_once(*this, s);
        uint32_t ms = s.poll_ms ? s.poll_ms : 1000;
        ::theia::runtime::send_after(::theia::runtime::process_timers(), ms,
                                     *this, "poll");
    }
}

// RegisterTimeSource (registration idiom, step 1): idempotent UPSERT — a
// restarted provider re-registers and keeps feeding; a re-registration updates
// the advertised priority/rate but keeps the latest observation (no gap).
TsyncReply TsyncCtl::handle_call(const RegisterTimeSourceReq& req,
                                 TsyncCtlState& s) {
    TsyncReply rep = system_services_tsync_TsyncReply_init_zero;
    if (!req.name[0]) {
        rep.status = 1;
        std::snprintf(rep.message, sizeof(rep.message), "empty source name");
        return rep;
    }
    auto& src = s.sources[req.name];
    src.kind                 = req.kind;
    src.priority             = req.priority;
    src.expected_interval_ms = req.expected_interval_ms ? req.expected_interval_ms
                                                        : 1000;
    log().info(std::string("time source registered: ") + req.name +
               " kind=" + (req.kind[0] ? req.kind : "?") +
               " prio=" + std::to_string(req.priority) +
               " interval_ms=" + std::to_string(src.expected_interval_ms) +
               " (" + std::to_string(s.sources.size()) + " registered)");
    std::snprintf(rep.message, sizeof(rep.message), "registered");
    return rep;
}

// TimeObservation cast (registration idiom, step 2): record the source's
// latest (utc, local) pair. An UNREGISTERED name is dropped loudly (fail fast
// — the provider must Register first; no implicit registration that would
// hide a mis-wired name), but only warned once per name via the table probe.
void TsyncCtl::handle_cast(const TimeObservation& msg, TsyncCtlState& s) {
    auto it = s.sources.find(msg.name);
    if (it == s.sources.end()) {
        log().warn(std::string("TimeObservation from unregistered source '") +
                   msg.name + "' dropped — Register first");
        return;
    }
    auto& src = it->second;
    src.utc_ns           = msg.utc_ns;
    src.local_ns         = msg.local_ns;
    src.uncertainty_ns   = msg.uncertainty_ns;
    src.pps_aligned      = msg.pps_aligned;
    src.last_obs_wall_ns = wall_now_ns();
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
    s.ptp_interface     = cfg.ptp_interface;
    s.ptp_domain        = cfg.ptp_domain;
    s.cfg_source        = cfg.source[0] ? cfg.source : "ptp";
    s.poll_ms           = cfg.poll_ms ? cfg.poll_ms : 1000;
    s.lock_offset_ns    = cfg.lock_offset_ns ? cfg.lock_offset_ns : 100000;
    s.discipline_clock  = cfg.discipline_clock;
    s.source_timeout_ms = cfg.source_timeout_ms ? cfg.source_timeout_ms : 3000;
    log().info(std::string("tsync config applied: iface=") + s.ptp_interface +
               " source=" + s.cfg_source + " poll_ms=" + std::to_string(s.poll_ms) +
               " discipline_clock=" + (s.discipline_clock ? "true" : "false") +
               " registered=" + std::to_string(s.sources.size()));
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

// ─── AUTOSAR ara::tsync getters ─────────────────────────────────────────────
//
// Thin per-facet wrappers over the SAME cached State that GetSyncStatus reads —
// no extra polling. A client that wants just one value (e.g. "am I LOCKED?")
// uses these instead of unpacking the whole snapshot.

// GetCurrentTimeSource → SYSTEM | PTP | EXTERNAL (which source backs the reference).
TimeSourceReply TsyncCtl::handle_call(const TimeSourceReq& /*req*/,
                                      TsyncCtlState& s) {
    TimeSourceReply rep = system_services_tsync_TimeSourceReply_init_zero;
    rep.source = static_cast<system_services_tsync_TimeSource>(s.source);
    return rep;
}

// GetSynchronizationState → UNAVAILABLE | UNLOCKED | HOLDOVER | LOCKED.
SyncStateReply TsyncCtl::handle_call(const SyncStateReq& /*req*/,
                                     TsyncCtlState& s) {
    SyncStateReply rep = system_services_tsync_SyncStateReply_init_zero;
    rep.state = static_cast<system_services_tsync_SyncState>(s.state);
    return rep;
}

// GetOffset → signed offset (ns) from the master. `valid` is false in
// UNAVAILABLE (no master to offset from) so a caller doesn't trust a 0 that
// means "no data" as a real 0ns lock.
OffsetReply TsyncCtl::handle_call(const OffsetReq& /*req*/,
                                  TsyncCtlState& s) {
    OffsetReply rep = system_services_tsync_OffsetReply_init_zero;
    rep.offset_ns = s.offset_ns;
    rep.valid = (s.state != 0 /*UNAVAILABLE*/);
    return rep;
}

// GetGrandmasterInfo → the GM clock identity + the disciplined NIC (both "" when
// there is no grandmaster — SYSTEM/EXTERNAL-only or no daemon).
GrandmasterReply TsyncCtl::handle_call(const GmInfoReq& /*req*/,
                                       TsyncCtlState& s) {
    GrandmasterReply rep = system_services_tsync_GrandmasterReply_init_zero;
    std::snprintf(rep.grandmaster, sizeof(rep.grandmaster), "%s", s.grandmaster.c_str());
    std::snprintf(rep.interface, sizeof(rep.interface), "%s", s.interface.c_str());
    return rep;
}

}  // namespace ara::tsync
