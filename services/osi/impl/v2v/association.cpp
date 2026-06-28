// Port of mesh_topo/estimator/association.py::TrackManager.

#include "association.hpp"

#include <cmath>
#include <set>

namespace ara::osi::v2v {

namespace {
constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
}

Eigen::Vector2d beacon_vel(const Beacon& b) {
    const double h = b.heading_deg * kDegToRad;
    return b.speed_mps * Eigen::Vector2d(std::cos(h), std::sin(h));
}

int TrackManager::new_track(double t, const Eigen::Vector2d& vel) {
    const int tr = next_++;
    track_last_[tr] = {t, vel};
    return tr;
}

std::map<std::string, int>
TrackManager::update(double t, const std::vector<Beacon>& frame) {
    std::map<std::string, int> mapping;
    std::vector<const Beacon*> unknown;

    // Known EIDs map straight through (and refresh their last-velocity).
    for (const auto& b : frame) {
        auto it = eid_track_.find(b.eid);
        if (it != eid_track_.end()) {
            mapping[b.eid] = it->second;
            track_last_[it->second] = {t, beacon_vel(b)};
        } else {
            unknown.push_back(&b);
        }
    }

    // Candidate "lost" tracks: not seen this frame, but recently alive.
    std::set<int> seen;
    for (const auto& kv : mapping) seen.insert(kv.second);
    std::vector<std::pair<int, Eigen::Vector2d>> lost;
    for (const auto& kv : track_last_) {
        const int tr = kv.first;
        const double tt = kv.second.first;
        if (seen.find(tr) == seen.end() && (t - tt) <= lost_after_s_)
            lost.emplace_back(tr, kv.second.second);
    }

    // Greedy velocity-continuity stitching of new EIDs to lost tracks.
    for (const Beacon* bp : unknown) {
        const Eigen::Vector2d v = beacon_vel(*bp);
        int best = -1;
        double best_d = gate_vel_;
        for (size_t k = 0; k < lost.size(); ++k) {
            const double d = (v - lost[k].second).norm();
            if (d < best_d) { best = static_cast<int>(k); best_d = d; }
        }
        int tr;
        if (best >= 0) {
            tr = lost[best].first;
            lost.erase(lost.begin() + best);
        } else {
            tr = new_track(t, v);
        }
        eid_track_[bp->eid] = tr;
        track_last_[tr] = {t, v};
        mapping[bp->eid] = tr;
    }

    // Retire EIDs that didn't appear (so a reused random EID can't collide).
    std::set<std::string> present;
    for (const auto& b : frame) present.insert(b.eid);
    for (auto it = eid_track_.begin(); it != eid_track_.end();) {
        if (present.find(it->first) == present.end() &&
            mapping.find(it->first) == mapping.end()) {
            it = eid_track_.erase(it);   // keep the track, drop the EID binding
        } else {
            ++it;
        }
    }
    return mapping;
}

}  // namespace ara::osi::v2v
