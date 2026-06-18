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
struct GnssFix {
    uint64_t utc_ns = 0;      // GPS UTC, epoch nanoseconds (0 when !valid)
    bool     valid  = false;  // a usable fix this poll
    double   lat    = 0.0;    // WGS84 degrees
    double   lon    = 0.0;
    double   alt    = 0.0;    // metres
    bool     rtk_fix = false; // RTK fixed/float (UBX path only; false for NMEA/fake)
    bool     pps     = false; // a PPS edge is available (/dev/pps0); false in v1
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
