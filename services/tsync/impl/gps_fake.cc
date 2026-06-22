// gps_fake — the DEFAULT GNSS variant (no --define). A synthetic receiver: it
// returns a valid fix whose UTC == CLOCK_REALTIME now, at a fixed location. So:
//   - the status path is fully exercised (source=GPS, state=LOCKED) on any host,
//   - the offset vs CLOCK_REALTIME is ~0, so even if discipline_clock were true
//     the step threshold is never crossed and the host clock is NOT touched.
// That makes Fake safe to run on a dev/CI box: a real clock step needs a real
// receiver (gps_rtk / gps_nmea) whose UTC actually differs from the host clock.
//
// Selected when no `--define gps=...` is passed (impl/BUILD.bazel default).

#include "impl/gps_backend.hpp"

#include <chrono>

namespace ara::tsync {

GnssFix GpsBackend::poll(const std::string& /*dev*/, uint32_t /*baud*/) {
    GnssFix f;
    f.valid = true;
    f.utc_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    // A plausible fixed position (a dev rig). Not used for time;
    // present so the Position API has something to serve.
    f.lat = 48.8369;
    f.lon = 8.9706;
    f.alt = 480.0;
    f.rtk_fix = false;
    f.pps = false;
    f.note = "fake GNSS (utc=host clock, no step)";
    return f;
}

const char* GpsBackend::name() { return "fake"; }

}  // namespace ara::tsync
