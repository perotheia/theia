// V2V SLAM — lift rotating ephemeral IDs into persistent tracks.
//
// Port of mesh_topo/estimator/association.py. The estimator never sees true ids.
// As long as an EID persists it maps to a stable track. When EIDs rotate, a
// newly-appeared EID is stitched to a recently-lost track by continuity of the
// GLOBAL-frame velocity vector (heading+speed) — the cue that survives rotation —
// via greedy nearest assignment under a gating threshold. With rotation off this
// reduces to the identity map. (HANDOFF §3 stage 1.)

#pragma once

#include <map>
#include <string>
#include <vector>

#include <Eigen/Dense>

#include "beacon.hpp"

namespace ara::osi::v2v {

// Global-frame velocity vector of a beacon: speed * (cos h, sin h), h in rad.
Eigen::Vector2d beacon_vel(const Beacon& b);

class TrackManager {
public:
    explicit TrackManager(double gate_vel = 6.0, double lost_after_s = 3.0)
        : gate_vel_(gate_vel), lost_after_s_(lost_after_s) {}

    // Update with one frame's beacons at time t; returns eid -> track_id for
    // every beacon in the frame. Mirrors TrackManager.update() exactly.
    std::map<std::string, int> update(double t, const std::vector<Beacon>& frame);

private:
    int new_track(double t, const Eigen::Vector2d& vel);

    double gate_vel_;
    double lost_after_s_;
    std::map<std::string, int>                          eid_track_;   // active EID -> track
    std::map<int, std::pair<double, Eigen::Vector2d>>   track_last_;  // track -> (t, vel)
    int                                                 next_ = 0;
};

}  // namespace ara::osi::v2v
