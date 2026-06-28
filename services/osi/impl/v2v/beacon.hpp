// V2V relative-topology SLAM — input data contract.
//
// Port of mesh_topo/beacons.py (the Python behavioural reference). Field names
// map 1:1. A beacon carries NO GPS / NO position / NO persistent identity — only
// a (rotating) ephemeral id, global-frame heading + speed, and RSSI to the
// strongest-K neighbours. The estimator reconstructs 2D relative positions from a
// sliding window of these. See tts/HANDOFF_CPP_V2V.md §2.
//
// This header is BACKEND-AGNOSTIC (no Theia/runtime deps) so the estimator is
// unit-testable in isolation. The Meshtastic transport decodes the compact on-air
// wire form (tts/README.md) into these doubles at its seam.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ara::osi::v2v {

// A plain 2D point — the PUBLIC result type (keeps Eigen OUT of the estimator's
// public header, so it doesn't leak into the node state / lib / main TUs; the
// internal pipeline uses Eigen in the .cpp). x→North, y→East in the local frame.
struct Vec2 {
    double x = 0.0;
    double y = 0.0;
};

// One heard neighbour: its ephemeral id + the measured RSSI (dBm).
struct NeighborObs {
    std::string neighbor_eid;
    double      rssi = 0.0;   // dBm, as measured
};

// A single beacon. heading_deg + speed_mps MUST be in a shared GLOBAL frame
// (e.g. true north) — that is what pins global rotation and stitches rotating
// ids. `true_id` is simulator-only ground truth: the estimator NEVER reads it.
struct Beacon {
    double                    t = 0.0;            // timestamp (s)
    std::string               eid;                // sender ephemeral id (may rotate)
    uint64_t                  seq = 0;            // sender seq no. (not used by the math)
    double                    heading_deg = 0.0;  // global-frame heading (deg)
    double                    speed_mps = 0.0;    // speed (m/s)
    std::vector<NeighborObs>  neighbors;          // strongest-K (K=6 default)
    int                       true_id = -1;       // sim-only eval; DO NOT pass to estimator
};

}  // namespace ara::osi::v2v
