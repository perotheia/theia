// gps_backend — the in-process GNSS time-source SEAM. The FC ACQUIRES UTC + a
// fix from a custom driver (NOT gpsd: no fork/exec of a 3rd-party binary), then
// TsyncCtl disciplines CLOCK_REALTIME from it. Exactly ONE variant .cc is
// compiled in via a bazel select() (impl/BUILD.bazel) — same module-inclusion
// pattern as shwa_backend.hpp (shwa_host.cc vs shwa_jetson.cc):
//
//   //rules/config:gps_rtk   → gps_rtk.cc   (UBX-NAV-PVT off /dev/serial0, ZED-F9R)
//   //rules/config:gps_nmea  → gps_nmea.cc  ($GxRMC/$GxGGA UTC off a serial tty)
//   (default, no --define)   → gps_fake.cc  (synthetic UTC @ the poll rate)
//
// Build selection: `bazel build //services/tsync/... --define gps=rtk`.
//
// The driver is STATELESS across calls in v1 (open-read-parse-close per poll) so
// the seam stays a single `poll()` — good enough at a ~1Hz tick. A real high-rate
// receiver would hold the fd open; that's a hardware-rig refinement.

#pragma once

#include <cstdint>
#include <string>

namespace ara::tsync {

// One GNSS reading. `utc_ns` is the receiver's UTC (epoch ns) at the fix; the
// caller compares it to CLOCK_REALTIME to decide whether to step the clock.
//
// Beyond UTC + position, UBX-NAV-PVT carries the receiver's NED velocity,
// ground speed and heading-of-motion (plus 1-sigma accuracies) in the SAME
// frame, so the RTK driver fills the velocity_/heading_ block at no extra read.
// The NMEA/fake variants leave it zeroed (velocity_valid = false). The FC turns
// this struct into ROS-shaped NavSatFix (position) + Odometry/TwistWithCovariance
// (velocity), using the accuracies as the covariance diagonals.
struct GnssFix {
    uint64_t utc_ns = 0;      // GPS UTC, epoch nanoseconds (0 when !valid)
    bool     valid  = false;  // a usable fix this poll
    double   lat    = 0.0;    // WGS84 degrees
    double   lon    = 0.0;
    double   alt    = 0.0;    // metres (height above MSL)
    bool     rtk_fix = false; // RTK fixed/float (UBX path only; false for NMEA/fake)
    bool     pps     = false; // a PPS edge is available (/dev/pps0); false in v1

    // 1-sigma position accuracy (metres) — NAV-PVT hAcc/vAcc. 0 when unknown.
    double   h_acc_m = 0.0;   // horizontal
    double   v_acc_m = 0.0;   // vertical

    // Velocity (UBX-NAV-PVT). NED frame, metres/second. velocity_valid is false
    // for the NMEA/fake variants (and a fix that didn't resolve velocity).
    bool     velocity_valid  = false;
    double   vel_n           = 0.0;   // north  (m/s)
    double   vel_e           = 0.0;   // east   (m/s)
    double   vel_d           = 0.0;   // down   (m/s)
    double   ground_speed    = 0.0;   // 2-D ground speed (m/s)
    double   heading_deg     = 0.0;   // heading of motion, degrees (N=0, CW)
    double   speed_acc_m_s   = 0.0;   // 1-sigma speed accuracy (m/s), sAcc
    double   heading_acc_deg = 0.0;   // 1-sigma heading accuracy (deg), headAcc

    std::string note;         // human note for the status line / log
};

// The selected variant defines these two. `dev` is the serial/tty device the
// driver reads (ignored by FakeGPS); "" → the variant's own default.
class GpsBackend {
public:
    static GnssFix poll(const std::string& dev);
    static const char* name();   // "rtk" | "nmea" | "fake" — for the status line
};

}  // namespace ara::tsync
