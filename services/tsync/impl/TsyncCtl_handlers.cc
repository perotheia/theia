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
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include <pb_decode.h>

#include "TimerService.hh"   // post_info / send_after / process_timers
#include "ParamsConfig.hh"   // get_config() — gps_serial_dev / gps_baud / poll_ms

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

// TIPC EGRESS for the broadcasts: emit each published message as a Send trace
// record to the log[trace] firehose. The generated broadcast_* fan-out is
// IN-PROCESS only (subscriber callbacks); this is what carries the data OFF the
// node — over TIPC to log[trace], where any observer (tdb tracecat, the GUI, a
// fusion node on another machine) subscribes. Gated by the node's Tracer: zero
// cost until the supervisor flips tracing on for tsync_ctl (`tdb trace tsync`),
// then every NavSatFix/Odometry lands on the firehose, decoded by the .proto.
// Mirrors GenServer's auto-trace on a cast (encode_for_trace + tracer.emit).
template <class Msg>
void trace_emit(const Msg& m) noexcept {
    auto& tr = ::theia::runtime::tracer_for(TsyncCtl::kNodeName);
    if (!tr.enabled()) return;
    uint8_t scratch[256];
    uint16_t n = ::theia::runtime::encode_for_trace(
        m, scratch, static_cast<uint16_t>(sizeof(scratch)));
    tr.emit(::theia::runtime::TraceEvent::Send,
            ::theia::runtime::msg_type_name<Msg>(),
            ::theia::runtime::next_trace_corr_id(), scratch, n);
}

// --- RTK-native GNSS broadcasts -------------------------------------------
//
// Re-publish a valid fix as the RTK-native GnssSolution (position + velocity +
// RTK quality, ONE message — supersedes the former NavSatFix+Odometry pair) and,
// when the receiver fuses attitude, an Imu (orientation from the F9R NAV-ATT
// solution). Both are senderReceiver PG broadcasts; the in-process broadcast_*
// fan-out delivers to subscribers; trace_emit (above) carries them OVER TIPC to
// the log[trace] firehose (tdb tracecat, GUI, a cross-machine fusion node). The
// covariance diagonals come from the receiver's reported 1-sigma accuracies
// (variance = sigma^2); off-diagonals stay 0.
void publish_gnss(TsyncCtl& self, const GnssFix& fix, uint64_t now) {
    // ---- GnssSolution (position + velocity + RTK quality) ----
    GnssSolution sol = platform_msgs_nav_GnssSolution_init_zero;
    sol.has_header = true;
    sol.header.timestamp_ns = fix.utc_ns ? fix.utc_ns : now;
    std::snprintf(sol.header.frame_id, sizeof(sol.header.frame_id), "gps");

    // Source: the RTK backend reads the receiver binary (UBX); NMEA/fake report
    // their own kind via GpsBackend::name().
    const char* bname = GpsBackend::name();
    if (std::strcmp(bname, "rtk") == 0)
        sol.solution_source = platform_msgs_nav_GnssSolutionSource_GnssSolutionSource_SOLUTION_SOURCE_BINARY;
    else if (std::strcmp(bname, "nmea") == 0)
        sol.solution_source = platform_msgs_nav_GnssSolutionSource_GnssSolutionSource_SOLUTION_SOURCE_NMEA;
    else
        sol.solution_source = platform_msgs_nav_GnssSolutionSource_GnssSolutionSource_SOLUTION_SOURCE_COMPUTED;

    // Fix status: rtk_fix → FIX (integer-resolved); else a single-point solution.
    // (The v1 backend struct does not distinguish FLOAT/DGPS/SBAS — a hardware-rig
    // refinement; FIX vs SINGLE is the field-meaningful split here.)
    sol.status = fix.rtk_fix
        ? platform_msgs_nav_GnssSolutionStatus_GnssSolutionStatus_STATUS_FIX
        : platform_msgs_nav_GnssSolutionStatus_GnssSolutionStatus_STATUS_SINGLE;

    sol.latitude  = fix.lat;
    sol.longitude = fix.lon;
    sol.altitude  = fix.alt;

    // ENU position + its 3x3 row-major covariance from hAcc (E,N) / vAcc (U).
    // pos_enu stays 0 in v1 (no local origin established) — a localizer fuses the
    // geodetic lat/lon/alt; the covariance still describes the position quality.
    sol.has_pos_enu = true;             // E,N,U all 0 until an ENU origin is set
    sol.pos_enu_cov_count = 9;
    const double hv = fix.h_acc_m * fix.h_acc_m;
    const double vv = fix.v_acc_m * fix.v_acc_m;
    sol.pos_enu_cov[0] = hv;   // E
    sol.pos_enu_cov[4] = hv;   // N
    sol.pos_enu_cov[8] = vv;   // U

    if (fix.velocity_valid) {
        // ENU velocity (m/s): E,N,U(=-down). vel_enu_cov diagonal from sAcc, split
        // isotropically across the 3 axes (the receiver reports a scalar sAcc).
        sol.has_vel_enu = true;
        sol.vel_enu.x = fix.vel_e;   // East
        sol.vel_enu.y = fix.vel_n;   // North
        sol.vel_enu.z = -fix.vel_d;  // Up
        sol.vel_enu_cov_count = 9;
        const double sv = fix.speed_acc_m_s * fix.speed_acc_m_s;
        sol.vel_enu_cov[0] = sv;
        sol.vel_enu_cov[4] = sv;
        sol.vel_enu_cov[8] = sv;
    }
    self.broadcast_gnss_solution_solution(sol);
    trace_emit(sol);   // → log[trace] firehose (tdb tracecat) when tracing is on

    // ---- Imu (orientation from the receiver's fused attitude) ----
    // Only the RTK backend with sensor fusion (ZED-F9R NAV-ATT) yields attitude;
    // the F9P/NMEA/fake have none → no Imu published (an autonomy stack treats a
    // missing Imu as "no inertial solution this tick"). The GnssFix struct carries
    // the FUSED orientation, not raw gyro/accel samples, so angular_velocity and
    // linear_acceleration are marked unknown (-1 leading covariance, ROS rule).
    if (fix.attitude_valid) {
        Imu im = platform_msgs_sensor_Imu_init_zero;
        im.has_header = true;
        im.header.timestamp_ns = fix.utc_ns ? fix.utc_ns : now;
        std::snprintf(im.header.frame_id, sizeof(im.header.frame_id), "gps");

        // Orientation from NAV-ATT roll/pitch/heading. ROS uses ENU + a yaw
        // measured CCW from East, while u-blox heading is CW from North — convert
        // (yaw_enu = 90° - heading). Quaternion from the Z-Y-X (yaw-pitch-roll)
        // Euler sequence.
        const double deg2rad = 3.14159265358979323846 / 180.0;
        const double roll  = fix.roll_deg  * deg2rad;
        const double pitch = fix.pitch_deg * deg2rad;
        const double yaw   = (90.0 - fix.att_heading_deg) * deg2rad;
        const double cr = std::cos(roll * 0.5),  sr = std::sin(roll * 0.5);
        const double cp = std::cos(pitch * 0.5), sp = std::sin(pitch * 0.5);
        const double cy = std::cos(yaw * 0.5),   sy = std::sin(yaw * 0.5);
        im.has_orientation = true;
        im.orientation.w = cr * cp * cy + sr * sp * sy;
        im.orientation.x = sr * cp * cy - cr * sp * sy;
        im.orientation.y = cr * sp * cy + sr * cp * sy;
        im.orientation.z = cr * cp * sy - sr * sp * cy;
        // 3x3 orientation covariance from the NAV-ATT 1-sigma accuracies (rad^2),
        // r,p,y on the diagonal.
        im.orientation_covariance_count = 9;
        const double rr = fix.roll_acc_deg * deg2rad, pp = fix.pitch_acc_deg * deg2rad,
                     yy = fix.heading_att_acc_deg * deg2rad;
        im.orientation_covariance[0] = rr * rr;   // roll
        im.orientation_covariance[4] = pp * pp;   // pitch
        im.orientation_covariance[8] = yy * yy;   // yaw
        // No raw rate/accel in the fused solution → mark both unknown (ROS: a
        // leading -1 in the covariance means "this field is not provided").
        im.has_angular_velocity = true;           // zeroed
        im.angular_velocity_covariance_count = 9;
        im.angular_velocity_covariance[0] = -1.0;
        im.has_linear_acceleration = true;        // zeroed
        im.linear_acceleration_covariance_count = 9;
        im.linear_acceleration_covariance[0] = -1.0;
        self.broadcast_imu_imu(im);
        trace_emit(im);   // → log[trace] firehose when tracing is on
    }

    if (!fix.velocity_valid) return;

    // Visibility for a live (in-car) test: a concise line per published fix.
    // INFO so it lands in the supervisor/stderr log without a wired subscriber.
    // Rate is the poll cadence (~1 Hz default), so it's not spammy.
    char line[256];
    std::snprintf(line, sizeof(line),
        "GNSS fix: lat=%.7f lon=%.7f alt=%.1fm %s hAcc=%.2fm | "
        "vel(N,E,D)=%.2f,%.2f,%.2f m/s spd=%.2f hdg=%.1f%s",
        fix.lat, fix.lon, fix.alt, fix.rtk_fix ? "RTK" : "3D", fix.h_acc_m,
        fix.vel_n, fix.vel_e, fix.vel_d, fix.ground_speed, fix.heading_deg,
        fix.attitude_valid ? "" : " (no-att)");
    self.log().info(line);
    if (fix.attitude_valid) {
        char att[160];
        std::snprintf(att, sizeof(att),
            "GNSS att: roll=%.2f pitch=%.2f heading=%.2f deg "
            "(acc r=%.2f p=%.2f h=%.2f)",
            fix.roll_deg, fix.pitch_deg, fix.att_heading_deg,
            fix.roll_acc_deg, fix.pitch_acc_deg, fix.heading_att_acc_deg);
        self.log().info(att);
    }
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
    GnssFix fix = GpsBackend::poll(s.gps_serial_dev, s.gps_baud);
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

// init: read the static deploy params (config/tsync.json), then schedule the
// first poll tick (requires_timers gives us send_after).
void TsyncCtl::init(TsyncCtlState& s) {
    auto cfg = ::theia::runtime::get_config().node(TsyncCtl::kNodeName);
    s.gps_serial_dev = cfg.str("gps_serial_dev", "");        // "" → driver default
    s.gps_baud       = cfg.u32("gps_baud", 0);               // 0 → driver default
    s.poll_ms        = cfg.u32("poll_ms", s.poll_ms);        // 100 (10 Hz) default
    log().info(std::string("tsync up — GNSS driver=") + GpsBackend::name() +
               " dev=" + (s.gps_serial_dev.empty() ? "(default)" : s.gps_serial_dev) +
               " baud=" + (s.gps_baud ? std::to_string(s.gps_baud) : "(default)") +
               " poll_ms=" + std::to_string(s.poll_ms) +
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
