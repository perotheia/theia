// State struct for OsiV2v — APP-OWNED, WRITE-ONCE.
//
// Holds the sliding window of decoded beacons (one frame per beacon-interval),
// the stateful TopologyEstimator (its TrackManager stitches rotating ids across
// solves), the applied V2vConfig, and the last-broadcast constellation (so the
// solve diffs against it to send only CHANGED vertices, NodeEdge-style, and so
// GetConstellation can serve a full snapshot).

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "impl/v2v/beacon.hpp"
#include "impl/v2v/estimator.hpp"
#include "impl/v2v/consensus.hpp"          // AlertConsensus (HANDOFF2)
#include "system/services/osi/osi.pb.h"   // ConstellationVertex (raw pb cache)

namespace ara::osi {

struct OsiV2vState {
    // ---- applied config (from V2vConfig / on_config_update) ----
    uint32_t window            = 10;
    uint32_t k_neighbors       = 6;
    double   beacon_interval_s = 5.0;
    // Estimator config (a_dbm/n_exp/sigmas/anchor/huber). Rebuilt into `est` when
    // it changes; the TrackManager is reset on a config change so gate/lost take
    // effect cleanly.
    ::ara::osi::v2v::EstConfig est_cfg;
    double   gate_vel     = 6.0;
    double   lost_after_s = 3.0;

    // ---- the estimator (STATEFUL: TrackManager persists across solves) ----
    std::shared_ptr<::ara::osi::v2v::TopologyEstimator> est;

    // ---- cooperative-alert consensus (HANDOFF2) — this vehicle's per-topic
    //      belief, fused from heard beacon alerts each round. Independent of the
    //      SLAM estimate; shares only the heard-beacon stream. ----
    ::ara::osi::v2v::ConsensusConfig con_cfg;
    std::shared_ptr<::ara::osi::v2v::AlertConsensus> con;
    double con_t = 0.0;   // last consensus round time (monotone beacon clock)

    // ---- the beacon window: one frame (vector<Beacon>) per beacon tick ----
    // Beacons accrued since the last frame boundary are flushed into `frames` on
    // the solve tick (one frame per tick, matching the beacon-interval cadence).
    // A solve consumes the latest `window` frames.
    std::vector<std::vector<::ara::osi::v2v::Beacon>> frames;
    std::vector<double>                               frame_times;
    std::vector<::ara::osi::v2v::Beacon>              pending;

    // ---- last broadcast constellation (for the partial-update diff + getter) ----
    uint64_t generation   = 0;
    int      anchor_track = -1;
    // track_id -> last emitted vertex (raw pb): serves GetConstellation + the
    // change-diff (emit only when a track's pose moves beyond an epsilon).
    std::map<int, system_services_osi_ConstellationVertex> last_vertices;
};

}  // namespace ara::osi
