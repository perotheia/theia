// V2V SLAM — relative-topology estimator over a sliding window of beacons.
//
// Port of mesh_topo/estimator/estimator.py::TopologyEstimator. Pipeline:
//   beacons -> association (tracks) -> odom shapes -> reduced offset LM
//           -> global reflection resolve -> full per-frame factor graph
//           -> refinement guard -> per-track latest position + velocity.
// Output is 2D positions in a shared LOCAL frame (no global coords). Global
// rotation is pinned by shared-frame heading odometry; translation by one anchor
// prior. See tts/HANDOFF_CPP_V2V.md §3-5; defaults = §4 table.
//
// KNOWN LIMITATION — residual LOCAL mirroring is a fundamental limit of range +
// heading/speed data (HANDOFF §6), NOT a bug.
//
// PUBLIC HEADER IS EIGEN-FREE: the result uses the plain Vec2 POD and the
// estimator hides its Eigen state behind an Impl pointer, so this header can be
// pulled into the node state / lib / main TUs without an Eigen include there.

#pragma once

#include <map>
#include <memory>
#include <vector>

#include "beacon.hpp"        // Beacon, Vec2 (both Eigen-free)

namespace ara::osi::v2v {

struct EstConfig {
    double A = -40.0;            // path-loss intercept (dBm at 1 m) — re-tune per radio
    double n = 2.8;             // path-loss exponent
    double sigma_rssi = 4.5;    // RSSI noise (dB); range-factor whitening
    double sigma_odom = 1.5;    // per-step odometry trust (m)
    double anchor_sigma = 0.5;  // translation anchor strength (m)
    double weak_prior_sigma = 1e3;  // weak prior on every var (numerical)
    double huber_delta = 6.0;   // robust cap on all residuals
};

struct EstimateResult {
    // Per-track latest-frame position + velocity (the headline output), Vec2 POD.
    std::map<int, Vec2> positions;
    std::map<int, Vec2> velocities;
    int    anchor = -1;         // the gauge anchor track (origin)
    double cost = 0.0;          // final graph robust cost
};

// Eigen-free, header-stable facade. The heavy Eigen pipeline + the stateful
// TrackManager live in the .cpp (behind Impl), so including this header costs no
// Eigen dependency on the consumer.
class TopologyEstimator {
public:
    explicit TopologyEstimator(EstConfig cfg = {});
    ~TopologyEstimator();
    TopologyEstimator(TopologyEstimator&&) noexcept;
    TopologyEstimator& operator=(TopologyEstimator&&) noexcept;

    // One window: frames[f] = beacons heard at times[f]. The internal TrackManager
    // is STATEFUL across calls (it stitches rotating ids over time). Returns the
    // per-track latest pose.
    EstimateResult estimate(const std::vector<std::vector<Beacon>>& frames,
                            const std::vector<double>& times);

    const EstConfig& config() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ara::osi::v2v
