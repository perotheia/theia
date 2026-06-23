// gps_fake — the DEFAULT GNSS variant (no --define). A synthetic receiver that
// returns a valid fix whose UTC == CLOCK_REALTIME now. So:
//   - the status path is fully exercised (source=GPS, state=LOCKED) on any host,
//   - the offset vs CLOCK_REALTIME is ~0, so even if discipline_clock were true
//     the step threshold is never crossed and the host clock is NOT touched.
// That makes Fake safe to run on a dev/CI box: a real clock step needs a real
// receiver (gps_rtk / gps_nmea) whose UTC actually differs from the host clock.
//
// MOTION: the fake drives a slow, continuously-turning track from a Weissach
// anchor so a consumer sees a MOVING ego — position advances AND velocity/heading
// are non-zero and sweep over time. This exercises the full GPS+odometry path
// (NavSatFix position + Odometry twist velocity → speed/heading) end-to-end, not
// just a dead pin. velocity_valid=true so tsync fills Odometry.twist.linear.
//
// Selected when no `--define gps=...` is passed (impl/BUILD.bazel default).

#include "impl/gps_backend.hpp"

#include <chrono>
#include <cmath>

namespace ara::tsync {

namespace {
// Track parameters: a gentle ~10 m/s drive on a large circle so heading sweeps
// 360° over the loop period. Small radius keeps it near the anchor on the map.
constexpr double kAnchorLat   = 48.8369;     // Weissach
constexpr double kAnchorLon   = 8.9706;
constexpr double kAnchorAlt   = 480.0;
constexpr double kSpeedMps    = 10.0;        // ground speed
constexpr double kLoopSeconds = 120.0;       // one full heading sweep per 2 min
constexpr double kPi          = 3.14159265358979323846;
// metres-per-degree at this latitude (WGS84 approx).
constexpr double kMetersPerDegLat = 111320.0;

// Process start time, captured on the first poll, so t advances from 0.
std::chrono::steady_clock::time_point g_start{};
bool g_started = false;
}  // namespace

GnssFix GpsBackend::poll(const std::string& /*dev*/, uint32_t /*baud*/) {
    GnssFix f;
    f.valid = true;
    f.utc_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    const auto now = std::chrono::steady_clock::now();
    if (!g_started) { g_start = now; g_started = true; }
    const double t = std::chrono::duration<double>(now - g_start).count();  // s

    // Heading sweeps 0→360° over kLoopSeconds (compass: N=0, CW). The ego drives
    // forward along that heading at kSpeedMps, so it traces a circle.
    const double heading_deg = std::fmod(t / kLoopSeconds * 360.0, 360.0);
    const double hd_rad      = heading_deg * kPi / 180.0;

    // NED/compass velocity for this heading + speed.
    const double vel_n = kSpeedMps * std::cos(hd_rad);   // north component
    const double vel_e = kSpeedMps * std::sin(hd_rad);   // east  component

    // Integrate position: distance travelled along the (turning) heading. For a
    // circle of this speed/period the radius is small; integrate the velocity
    // components over t to get the offset from the anchor (closed-form circle).
    //   x_e(t) = ∫ v·sin(θ) dt,  x_n(t) = ∫ v·cos(θ) dt,  θ = ω t, ω = 2π/T
    const double omega = 2.0 * kPi / kLoopSeconds;       // rad/s
    // ∫₀ᵗ v sin(ωτ)dτ = v/ω (1 - cos ωt);  ∫₀ᵗ v cos(ωτ)dτ = v/ω sin ωt
    const double off_e = kSpeedMps / omega * (1.0 - std::cos(omega * t));  // m east
    const double off_n = kSpeedMps / omega * std::sin(omega * t);          // m north

    const double mPerDegLon = kMetersPerDegLat * std::cos(kAnchorLat * kPi / 180.0);
    f.lat = kAnchorLat + off_n / kMetersPerDegLat;
    f.lon = kAnchorLon + off_e / mPerDegLon;
    f.alt = kAnchorAlt;
    f.rtk_fix = false;
    f.pps = false;

    // Velocity model — what makes Odometry (speed + heading) come alive.
    f.velocity_valid = true;
    f.vel_n          = vel_n;
    f.vel_e          = vel_e;
    f.vel_d          = 0.0;
    f.ground_speed   = kSpeedMps;
    f.heading_deg    = heading_deg;
    f.note = "fake GNSS (moving track; utc=host clock, no step)";
    return f;
}

const char* GpsBackend::name() { return "fake"; }

}  // namespace ara::tsync
