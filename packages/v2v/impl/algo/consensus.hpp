// Cooperative-alert consensus — per-topic binary agreement via broadcast
// Gaussian BP over the anonymous vehicle mesh (HANDOFF2 / tts consensus.py).
//
// Each vehicle holds, PER TOPIC, one scalar belief x ∈ [0,1] as a Gaussian
// (mu, lam) in information form (eta = lam·mu). A witness carries a tight prior
// at 0/1; everyone else a weak prior at 0.5. Beliefs ride the beacon; each
// receiver fuses heard beliefs as Gaussian messages through a smoothness
// potential, capped by evidence provenance (§5.1 — the load-bearing decision:
// a crowd of any size must lose to one direct observation). Agreement emerges in
// a few beacon rounds; the network re-converges when witnesses change.
//
// Scalar arithmetic only — no Eigen, no solver, no factor graph. One
// AlertConsensus instance per vehicle holds every topic it participates in.
// It reuses the estimator's Stage-1 TrackManager (shared per vehicle) to key
// neighbours across EID rotation ("stitch" mode); raw-EID keying is the fallback.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "beacon.hpp"        // Beacon, AlertBelief (Eigen-free)

namespace ara::osi::v2v {

// Tuned defaults — HANDOFF2 §4. The caps are LOAD-BEARING (§5.1):
// (expected live neighbours) × cap_hearsay < witness prior lam. Do NOT retune
// past that invariant, or the network converges once and can never change its
// mind.
struct ConsensusConfig {
    double sigma_witness    = 0.05;   // witness prior sigma (lam 400)
    double sigma_uninformed = 2.0;    // uninformed prior sigma at 0.5
    double sigma_consensus  = 0.1;    // smoothness potential
    double cap_witness      = 300.0;  // witness-message precision cap
    double cap_hearsay      = 30.0;   // relayed-message precision cap (§5.1)
    double damping          = 0.5;    // EMA weight of the new message
    double lost_after_s     = 15.0;   // message expiry = 3 × beacon interval (5 s)
    bool   stitch           = false;  // key neighbours by track (true) or raw EID
};

// One topic's readable state.
struct AlertState {
    uint8_t topic = 0;
    double  mu = 0.5;
    double  lam = 0.25;
    bool    decision = false;   // mu > 0.5 — the precision-weighted majority vote
    bool    witness = false;    // am I a direct witness of this topic
};

// The consensus state ONE vehicle carries across all its topics. Sees only heard
// beacons. Eigen-free, header-stable (the TrackManager, if used, lives behind
// Impl so this header stays dependency-light).
class AlertConsensus {
public:
    explicit AlertConsensus(const ConsensusConfig& cfg = {});
    ~AlertConsensus();
    AlertConsensus(AlertConsensus&&) noexcept;
    AlertConsensus& operator=(AlertConsensus&&) noexcept;

    // Declare/refresh this vehicle's OWN observation of a topic. `decision` is the
    // observed binary state (present/absent); passing witness=false makes it an
    // uninformed participant (weak prior at 0.5). Overwrites the prior (§3.1).
    void observe(uint8_t topic, bool decision, bool witness = true);

    // One beacon round at time t: fuse every heard neighbour beacon's alert
    // beliefs (§3.3), then recompute each topic's broadcastable belief (§3.4).
    void step(double t, const std::vector<Beacon>& heard);

    // This vehicle's current belief/decision on a topic (§3.4).
    AlertState state(uint8_t topic, double t) const;

    // The alert beliefs to piggyback on THIS vehicle's outgoing beacon this round
    // (one per topic it participates in) — what §3.4 broadcasts.
    std::vector<AlertBelief> broadcast(double t) const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace ara::osi::v2v
