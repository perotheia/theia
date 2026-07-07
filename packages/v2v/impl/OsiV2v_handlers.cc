// User handler bodies for OsiV2v — the V2V relative-topology SLAM node.
//
// Consumes Beacons (from the Meshtastic transport over V2vBeaconStream), runs
// the windowed factor-graph SLAM estimator (impl/v2v/), holds the constellation
// STATE (GetConstellation serves it), and BROADCASTS vertex-format partial
// updates ON CHANGE (ConstellationStream). Per-vertex node-metrics: estimated
// distance to the gauge anchor + velocity vector in NED. See package.art.

#include "lib/OsiV2v.hh"

#include "TimerService.hh"   // post_info / send_after / process_timers
#include "ParamsConfig.hh"   // get_config().node(kNodeName).<type>(key, dflt)

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "algo/beacon.hpp"
#include "algo/estimator.hpp"

namespace ara::v2v {

namespace {

uint64_t now_ns_() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// nanopb Beacon (fixed char[] fields, per osi.options) → internal v2v::Beacon.
::ara::osi::v2v::Beacon decode_beacon(const Beacon& b) {
    ::ara::osi::v2v::Beacon out;
    out.t           = b.t;
    out.eid         = b.eid;        // char[] — direct
    out.seq         = b.seq;
    out.heading_deg = b.heading_deg;
    out.speed_mps   = b.speed_mps;
    for (pb_size_t i = 0; i < b.neighbors_count && i < 16; ++i) {
        out.neighbors.push_back({std::string(b.neighbors[i].neighbor_eid),
                                 b.neighbors[i].rssi});
    }
    // HANDOFF2 §2 — the piggybacked alert beliefs (one per topic).
    for (pb_size_t i = 0; i < b.alerts_count && i < 8; ++i) {
        ::ara::osi::v2v::AlertBelief a;
        a.topic   = static_cast<uint8_t>(b.alerts[i].topic);
        a.mu      = b.alerts[i].mu;
        a.lam     = b.alerts[i].lam;
        a.witness = b.alerts[i].witness;
        out.alerts.push_back(a);
    }
    return out;
}

}  // namespace

// init: load params, build the estimator, kick the solve tick.
void OsiV2v::init(OsiV2vState& s) {
    auto cfg = ::theia::runtime::get_config().node(kNodeName);
    s.window            = static_cast<uint32_t>(cfg.u32("window", 10));
    s.k_neighbors       = static_cast<uint32_t>(cfg.u32("k_neighbors", 6));
    s.beacon_interval_s = cfg.f64("beacon_interval_s", 5.0);
    s.gate_vel          = cfg.f64("gate_vel", 6.0);
    s.lost_after_s      = cfg.f64("lost_after_s", 3.0);
    s.est_cfg.A           = cfg.f64("a_dbm", -40.0);
    s.est_cfg.n           = cfg.f64("n_exp", 2.8);
    s.est_cfg.sigma_rssi  = cfg.f64("sigma_rssi", 4.5);
    s.est_cfg.sigma_odom  = cfg.f64("sigma_odom", 1.5);
    s.est_cfg.anchor_sigma = cfg.f64("anchor_sigma", 0.5);
    s.est_cfg.huber_delta = cfg.f64("huber_delta", 6.0);
    s.est = std::make_shared<::ara::osi::v2v::TopologyEstimator>(s.est_cfg);

    // Cooperative-alert consensus (HANDOFF2 §4). Defaults are the tuned values;
    // the caps are load-bearing (§5.1). Reuse the estimator's stitch keying so
    // alerts survive EID rotation.
    s.con_cfg.sigma_witness    = cfg.f64("c_sigma_witness", 0.05);
    s.con_cfg.sigma_uninformed = cfg.f64("c_sigma_uninformed", 2.0);
    s.con_cfg.sigma_consensus  = cfg.f64("c_sigma_consensus", 0.1);
    s.con_cfg.cap_witness      = cfg.f64("c_cap_witness", 300.0);
    s.con_cfg.cap_hearsay      = cfg.f64("c_cap_hearsay", 30.0);
    s.con_cfg.damping          = cfg.f64("c_damping", 0.5);
    s.con_cfg.lost_after_s     = cfg.f64("c_lost_after_s", 15.0);
    s.con_cfg.stitch           = true;   // key alerts by track (§3.2)
    s.con = std::make_shared<::ara::osi::v2v::AlertConsensus>(s.con_cfg);

    // Consume the Beacon group (Meshtastic produces it). pg_join routes each
    // multicast Beacon into this node's handle_cast(Beacon) below.
    pg_join<Beacon>();
    // Produce the ConstellationUpdate group (the GUI/firehose pg_join it).
    pg_watch<ConstellationUpdate>();

    log().info(std::string("osi v2v up — SLAM estimator "
        "(window=") + std::to_string(s.window) +
        ", K=" + std::to_string(s.k_neighbors) +
        ", interval=" + std::to_string(s.beacon_interval_s) + "s)");

    // First solve tick after one beacon interval.
    ::theia::runtime::send_after(::theia::runtime::process_timers(),
        static_cast<uint32_t>(s.beacon_interval_s * 1000.0), *this, "solve");
}

// handle_cast(Beacon): accrue the decoded beacon into the current frame bucket.
void OsiV2v::handle_cast(const Beacon& msg, OsiV2vState& s) {
    s.pending.push_back(decode_beacon(msg));
}

// handle_info "solve": close the current frame, run the windowed estimate, and
// broadcast only the vertices that CHANGED since the last solve. Reschedule.
void OsiV2v::handle_info(const char* info, OsiV2vState& s) {
    if (!info || std::strcmp(info, "solve") != 0) return;
    const uint32_t period_ms =
        static_cast<uint32_t>((s.beacon_interval_s > 0 ? s.beacon_interval_s : 5.0) * 1000.0);

    // Close the frame (even an empty one keeps the time axis monotone). The frame
    // time is the median pending beacon t, else a synthetic interval clock.
    const double t = s.frames.empty()
        ? 0.0
        : s.frame_times.back() + s.beacon_interval_s;
    if (!s.pending.empty()) {
        // Cooperative-alert consensus round: fuse THIS round's heard beacons'
        // alert beliefs before the pending frame is moved into the window. The
        // consensus clock is monotone in beacon intervals (§5.4 — timeouts are in
        // rounds, not wall time), so it stays consistent with lost_after_s.
        s.con_t += s.beacon_interval_s;
        if (s.con) s.con->step(s.con_t, s.pending);

        s.frames.push_back(std::move(s.pending));
        s.frame_times.push_back(s.frames.back().empty() ? t : s.frames.back().front().t);
        s.pending.clear();
    }
    // Slide the window: keep the latest `window` frames.
    while (s.frames.size() > s.window) {
        s.frames.erase(s.frames.begin());
        s.frame_times.erase(s.frame_times.begin());
    }

    if (!s.frames.empty()) {
        auto res = s.est->estimate(s.frames, s.frame_times);
        s.anchor_track = res.anchor;

        // Build the partial update: a vertex per track whose pose changed beyond
        // an epsilon (or is new). Anchor position = origin in the local frame; the
        // NED metric is the range to the anchor + the velocity mapped into NED.
        const ::ara::osi::v2v::Vec2 anchor_pos =
            res.positions.count(res.anchor) ? res.positions.at(res.anchor)
                                            : ::ara::osi::v2v::Vec2{};

        packages_v2v_ConstellationUpdate upd =
            packages_v2v_ConstellationUpdate_init_zero;
        upd.gen          = ++s.generation;
        upd.ts_ns        = now_ns_();
        upd.anchor_track = static_cast<uint32_t>(res.anchor < 0 ? 0 : res.anchor);
        pb_size_t nv = 0;
        constexpr double kEps = 0.5;   // metres; below this a track is "unchanged"

        for (const auto& kv : res.positions) {
            if (nv >= 64) break;
            const int tr = kv.first;
            const ::ara::osi::v2v::Vec2& p = kv.second;
            const ::ara::osi::v2v::Vec2& v = res.velocities.at(tr);

            packages_v2v_ConstellationVertex vert =
                packages_v2v_ConstellationVertex_init_zero;
            vert.op             = 0;   // UPSERT
            vert.track_id       = static_cast<uint32_t>(tr);
            vert.x = p.x; vert.y = p.y;
            vert.vx = v.x; vert.vy = v.y;
            // NED node-metric: estimated distance to the anchor + NED velocity.
            // Local frame is gauge-free; we map x→North, y→East (planar, d=0).
            vert.est_distance_m = std::hypot(p.x - anchor_pos.x, p.y - anchor_pos.y);
            vert.vel_n = v.x; vert.vel_e = v.y; vert.vel_d = 0.0;
            vert.mirrored = false;     // (rigid-vs-reflect detector: TODO telemetry)

            // Change-diff: emit only if new or moved > eps.
            auto prev = s.last_vertices.find(tr);
            const bool changed = (prev == s.last_vertices.end()) ||
                std::hypot(prev->second.x - vert.x, prev->second.y - vert.y) > kEps;
            s.last_vertices[tr] = vert;     // always cache for GetConstellation
            if (changed) upd.vertices[nv++] = vert;
        }

        // REMOVE vertices for tracks that vanished from the estimate.
        for (auto it = s.last_vertices.begin(); it != s.last_vertices.end();) {
            if (res.positions.count(it->first) == 0) {
                if (nv < 64) {
                    packages_v2v_ConstellationVertex rm =
                        packages_v2v_ConstellationVertex_init_zero;
                    rm.op = 1;   // REMOVE
                    rm.track_id = static_cast<uint32_t>(it->first);
                    upd.vertices[nv++] = rm;
                }
                it = s.last_vertices.erase(it);
            } else {
                ++it;
            }
        }
        upd.vertices_count = nv;

        // Broadcast ONLY when something changed (partial-update-on-change).
        if (nv > 0) broadcast_broadcast_upd(upd);
    }

    ::theia::runtime::send_after(::theia::runtime::process_timers(), period_ms,
                                 *this, "solve");
}

// on_config_update: apply V2vConfig live (rebuild the estimator).
void OsiV2v::on_config_update(
        const platform_runtime_ConfigUpdated& cfg, OsiV2vState& s) {
    packages_v2v_V2vConfig c = packages_v2v_V2vConfig_init_zero;
    auto stream = pb_istream_from_buffer(
        reinterpret_cast<const pb_byte_t*>(cfg.config.bytes), cfg.config.size);
    if (!pb_decode(&stream, packages_v2v_V2vConfig_fields, &c)) {
        log().warn("on_config_update: V2vConfig decode failed — ignored");
        return;
    }
    if (c.window)       s.window      = c.window;
    if (c.k_neighbors)  s.k_neighbors = c.k_neighbors;
    if (c.beacon_interval_s > 0) s.beacon_interval_s = c.beacon_interval_s;
    if (c.a_dbm)        s.est_cfg.A           = c.a_dbm;
    if (c.n_exp)        s.est_cfg.n           = c.n_exp;
    if (c.sigma_rssi)   s.est_cfg.sigma_rssi  = c.sigma_rssi;
    if (c.sigma_odom)   s.est_cfg.sigma_odom  = c.sigma_odom;
    if (c.anchor_sigma) s.est_cfg.anchor_sigma = c.anchor_sigma;
    if (c.huber_delta)  s.est_cfg.huber_delta = c.huber_delta;
    if (c.gate_vel)     s.gate_vel     = c.gate_vel;
    if (c.lost_after_s) s.lost_after_s = c.lost_after_s;
    // Rebuild the estimator (resets the TrackManager — gate/lost take effect).
    s.est = std::make_shared<::ara::osi::v2v::TopologyEstimator>(s.est_cfg);
    log().info("v2v config applied (estimator rebuilt)");
}

// GetConstellation: serve the full cached constellation (the getter).
ConstellationUpdate OsiV2v::handle_call(
        const GetConstellationReq& /*req*/, OsiV2vState& s) {
    packages_v2v_ConstellationUpdate out =
        packages_v2v_ConstellationUpdate_init_zero;
    out.gen          = s.generation;
    out.ts_ns        = now_ns_();
    out.anchor_track = static_cast<uint32_t>(s.anchor_track < 0 ? 0 : s.anchor_track);
    pb_size_t nv = 0;
    for (const auto& kv : s.last_vertices) {
        if (nv >= 64) break;
        out.vertices[nv++] = kv.second;
    }
    out.vertices_count = nv;
    return out;
}

// GetAlertDecision: this vehicle's current consensus belief/decision on a topic
// (HANDOFF2 §3.4). Read-only; the decision is mu > 0.5 (precision-weighted vote).
AlertDecision OsiV2v::handle_call(
        const GetAlertDecisionReq& req, OsiV2vState& s) {
    packages_v2v_AlertDecision out =
        packages_v2v_AlertDecision_init_zero;
    out.topic = req.topic;
    if (s.con) {
        auto st = s.con->state(static_cast<uint8_t>(req.topic), s.con_t);
        out.mu       = st.mu;
        out.lam      = st.lam;
        out.decision = st.decision;
        out.witness  = st.witness;
    } else {
        out.mu = 0.5;
    }
    return out;
}

// InjectBeacon: the TEST path — a probe injects a neighbour beacon over TIPC when
// the Meshtastic radio node is disabled (run_on_start=false). It lands in the same
// `pending` bucket as a radio-decoded beacon, so the next solve tick fuses its
// alert beliefs into consensus (and the beacon into the SLAM window) exactly as
// if the radio delivered it.
InjectBeaconReply OsiV2v::handle_call(
        const InjectBeaconReq& req, OsiV2vState& s) {
    packages_v2v_InjectBeaconReply out =
        packages_v2v_InjectBeaconReply_init_zero;
    s.pending.push_back(decode_beacon(req.beacon));
    out.accepted = true;
    return out;
}

}  // namespace ara::osi
