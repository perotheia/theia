// User handler bodies for TsyncCtl — the Time-Sync control node.
//
// The acquire-and-distribute control plane: on each tick it polls the in-process
// GNSS driver (impl/gps_backend.hpp — RTK/NMEA/Fake, chosen at build time) AND
// the ptp4l backend (impl/ptp_backend.hpp), fuses them into the latest SyncStatus
// (cached in State, served by GetSyncStatus), and logs a status line on a state
// edge (loss-of-lock → a health event for PHM). On central (config.discipline_-
// clock=true) it STEPS CLOCK_REALTIME from a GPS fix (clock_settime); phc2sys +
// ptp4l then distribute it. Graceful-degrades to UNAVAILABLE/SYSTEM when there's
// no GPS fix / no ptp4l (the dev host has neither). EPERM (no CAP_SYS_TIME) on the
// clock step is logged once and tolerated. See TSYNC-cluster-whide.md.

#include "lib/TsyncCtl.hh"

#include "impl/ptp_backend.hpp"
#include "impl/gps_backend.hpp"

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include <pb_decode.h>

#include "TimerService.hh"   // post_info / send_after / process_timers

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
// tolerated — the FC keeps reporting GPS time without owning the clock.
constexpr long long kStepThresholdNs = 100 * 1000000LL;   // 100 ms

void maybe_discipline(TsyncCtl& self, TsyncCtlState& s, uint64_t gps_utc_ns) {
    if (!s.discipline_clock) return;
    long long off = (long long)gps_utc_ns - (long long)wall_now_ns();
    long long mag = off < 0 ? -off : off;
    if (mag < kStepThresholdNs && s.gps_disciplined) return;  // already close
    struct timespec ts;
    ts.tv_sec  = (time_t)(gps_utc_ns / 1000000000ULL);
    ts.tv_nsec = (long)(gps_utc_ns % 1000000000ULL);
    if (::clock_settime(CLOCK_REALTIME, &ts) == 0) {
        s.gps_disciplined = true;
        self.log().info(std::string("disciplined CLOCK_REALTIME from GPS (stepped ") +
                        std::to_string(off) + "ns)");
    } else if (errno == EPERM) {
        if (!s.settime_eperm_logged) {
            self.log().warn("clock_settime: EPERM (no CAP_SYS_TIME) — reporting GPS "
                            "time but NOT stepping the host clock");
            s.settime_eperm_logged = true;
        }
    } else {
        self.log().warn(std::string("clock_settime failed: ") + std::strerror(errno));
    }
}

// --- ROS-shaped GNSS broadcasts -------------------------------------------
//
// Re-publish a valid fix as NavSatFix (position) + Odometry (velocity+heading)
// for a localization/fusion app. Both are senderReceiver broadcasts; the
// in-process broadcast_* fan-out delivers to subscribers. A camera/fusion FC
// that wants them over TIPC subscribes via the probe/com path (same model as
// shwa's AccelTelemetry). The covariance diagonals come from the receiver's
// reported 1-sigma accuracies (variance = sigma^2); off-diagonals stay 0 and
// covariance_type = APPROXIMATED.
void publish_gnss(TsyncCtl& self, const GnssFix& fix, uint64_t now) {
    // ---- NavSatFix (position) ----
    NavSatFix nf = platform_msgs_sensor_NavSatFix_init_zero;
    nf.has_header = true;
    nf.header.timestamp_ns = fix.utc_ns ? fix.utc_ns : now;
    std::snprintf(nf.header.frame_id, sizeof(nf.header.frame_id), "gps");
    nf.status    = fix.rtk_fix
        ? platform_msgs_sensor_NavSatStatus_NavSatStatus_STATUS_GBAS_FIX
        : platform_msgs_sensor_NavSatStatus_NavSatStatus_STATUS_FIX;
    nf.latitude  = fix.lat;
    nf.longitude = fix.lon;
    nf.altitude  = fix.alt;
    // 3x3 ENU covariance: variance on the diagonal from hAcc (E,N) / vAcc (U).
    nf.position_covariance_count = 9;
    const double hv = fix.h_acc_m * fix.h_acc_m;
    const double vv = fix.v_acc_m * fix.v_acc_m;
    nf.position_covariance[0] = hv;   // E
    nf.position_covariance[4] = hv;   // N
    nf.position_covariance[8] = vv;   // U
    nf.position_covariance_type =
        (fix.h_acc_m > 0.0)
            ? platform_msgs_sensor_NavSatCovarianceType_NavSatCovarianceType_COVARIANCE_TYPE_DIAGONAL_KNOWN
            : platform_msgs_sensor_NavSatCovarianceType_NavSatCovarianceType_COVARIANCE_TYPE_UNKNOWN;
    self.broadcast_navsatfix_fix(nf);

    if (!fix.velocity_valid) return;

    // ---- Odometry (velocity + heading) ----
    // pose carries the same geodetic position (WGS84, not a metric local frame in
    // v1); twist.linear is the NED velocity (m/s) in child_frame_id "gps".
    // twist.angular stays 0 (a single-antenna receiver reports no body rates).
    Odometry od = platform_msgs_nav_Odometry_init_zero;
    od.has_header = true;
    od.header.timestamp_ns = fix.utc_ns ? fix.utc_ns : now;
    std::snprintf(od.header.frame_id, sizeof(od.header.frame_id), "map");
    std::snprintf(od.child_frame_id, sizeof(od.child_frame_id), "gps");
    od.has_pose = true;
    od.pose.has_pose = true;
    od.pose.pose.has_position = true;
    od.pose.pose.position.x = fix.lon;   // geodetic; consumer fuses NavSatFix instead
    od.pose.pose.position.y = fix.lat;
    od.pose.pose.position.z = fix.alt;
    od.has_twist = true;
    od.twist.has_twist = true;
    od.twist.twist.has_linear = true;
    od.twist.twist.linear.x = fix.vel_e;   // ENU-ish: x=East, y=North, z=Up(-down)
    od.twist.twist.linear.y = fix.vel_n;
    od.twist.twist.linear.z = -fix.vel_d;
    od.twist.twist.has_angular = true;     // zeroed
    // 6x6 twist covariance: linear variances from sAcc (split across the 3 axes
    // as an isotropic approximation); angular unknown (-1 on the diagonal).
    od.twist.covariance_count = 36;
    const double sv = fix.speed_acc_m_s * fix.speed_acc_m_s;
    od.twist.covariance[0]  = sv;   // vx
    od.twist.covariance[7]  = sv;   // vy
    od.twist.covariance[14] = sv;   // vz
    od.twist.covariance[21] = -1.0; // roll rate unknown
    od.twist.covariance[28] = -1.0; // pitch rate unknown
    od.twist.covariance[35] = -1.0; // yaw rate unknown
    self.broadcast_gnss_odom_odom(od);
}

}  // namespace

namespace {

// One status poll → update State. ACQUIRE from GPS first (the source on central);
// if there's no GPS fix, fall through to the PTP backend (the slave lock on
// compute). Logs only on a STATE EDGE; a downward edge from LOCKED is the loss-of-
// lock event a PHM would escalate on.
void poll_once(TsyncCtl& self, TsyncCtlState& s) {
    const uint64_t now = wall_now_ns();

    // --- GNSS acquisition (the in-process driver) -----------------------------
    GnssFix fix = GpsBackend::poll(s.ptp_interface /*reuse as serial dev hint*/);
    if (fix.valid && fix.utc_ns) {
        s.last_fix_ns = now;
        maybe_discipline(self, s, fix.utc_ns);
        s.source      = T_GPS;
        s.state       = S_LOCKED;
        s.offset_ns   = (long long)fix.utc_ns - (long long)now;
        s.grandmaster = std::string("GNSS(") + GpsBackend::name() + ")" +
                        (fix.rtk_fix ? " RTK" : "");
        s.interface   = "";
        s.message     = fix.note;
        publish_gnss(self, fix, now);   // ROS-shaped NavSatFix + Odometry
    } else if (s.last_fix_ns && now - s.last_fix_ns <
               (uint64_t)s.gps_fix_timeout_ms * 1000000ULL) {
        // Recent fix lost momentarily — hold GPS HOLDOVER (free-run) until timeout.
        s.source  = T_GPS;
        s.state   = S_HOLDOVER;
        s.message = std::string("GNSS(") + GpsBackend::name() + ") holdover — " + fix.note;
    } else {
        // No GPS — report the PTP lock (compute slave) or degrade to the wall clock.
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
        static const char* kSrc[] = {"SYSTEM", "PTP", "GPS"};
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
    log().info(std::string("tsync up — GNSS driver=") + GpsBackend::name() +
               " + linuxptp distribution (discipline_clock=" +
               (s.discipline_clock ? "true" : "false") + ")");
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
    s.ptp_interface      = cfg.ptp_interface;
    s.ptp_domain         = cfg.ptp_domain;
    s.cfg_source         = cfg.source[0] ? cfg.source : "ptp";
    s.poll_ms            = cfg.poll_ms ? cfg.poll_ms : 1000;
    s.lock_offset_ns     = cfg.lock_offset_ns ? cfg.lock_offset_ns : 100000;
    s.discipline_clock   = cfg.discipline_clock;
    s.gps_fix_timeout_ms = cfg.gps_fix_timeout_ms ? cfg.gps_fix_timeout_ms : 3000;
    log().info(std::string("tsync config applied: iface=") + s.ptp_interface +
               " source=" + s.cfg_source + " poll_ms=" + std::to_string(s.poll_ms) +
               " discipline_clock=" + (s.discipline_clock ? "true" : "false") +
               " gps=" + GpsBackend::name());
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

// GetCurrentTimeSource → SYSTEM | PTP | NTP (which source backs the reference).
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
// there is no grandmaster — SYSTEM/NTP or no daemon).
GrandmasterReply TsyncCtl::handle_call(const GmInfoReq& /*req*/,
                                       TsyncCtlState& s) {
    GrandmasterReply rep = system_services_tsync_GrandmasterReply_init_zero;
    std::snprintf(rep.grandmaster, sizeof(rep.grandmaster), "%s", s.grandmaster.c_str());
    std::snprintf(rep.interface, sizeof(rep.interface), "%s", s.interface.c_str());
    return rep;
}

}  // namespace ara::tsync
