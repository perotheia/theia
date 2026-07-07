// AlertConsensus — port of tts/mesh_topo/consensus.py (HANDOFF2 §3). The math is
// the behavioural reference; match it, not the Python data structures.
//
// Information form throughout: eta = lam·mu. Per topic: a prior (eta0, lam0) and
// a per-neighbour-key store {eta, lam, t_last}. A round fuses each heard belief
// through the smoothness potential, caps it by provenance, EMA-damps it against
// the neighbour's previous message, then rebuilds the belief as prior + Σ(live
// store entries). Prune drops entries older than 4× lost_after.

#include "consensus.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <tuple>
#include <unordered_map>

#include "association.hpp"   // TrackManager (stitch keying — same Stage-1 as SLAM)

namespace ara::osi::v2v {
namespace {

// Pass an information-form Gaussian (eta,lam) through the x_i = x_j + noise
// smoothness potential (consensus.py::_through_smoothness). Returns (eta',lam').
inline std::pair<double, double> through_smoothness(double eta, double lam,
                                                    double sigma_c) {
    if (lam <= 1e-12) return {0.0, 0.0};
    const double var = 1.0 / lam + sigma_c * sigma_c;
    const double lam_out = 1.0 / var;
    return {lam_out * (eta / lam), lam_out};
}

}  // namespace

// One topic's state: prior + per-neighbour-key incoming messages.
struct TopicState {
    bool   witness = false;
    double prior_eta = 0.5 / (2.0 * 2.0);   // uninformed default (mu 0.5, sig 2)
    double prior_lam = 1.0 / (2.0 * 2.0);
    // key -> (eta, lam, t_last)
    std::unordered_map<std::string, std::tuple<double, double, double>> in;
};

struct AlertConsensus::Impl {
    ConsensusConfig cfg;
    std::map<uint8_t, TopicState> topics;
    // Stitch keying reuses the estimator's Stage-1 association, one instance per
    // vehicle fed with the beacons IT hears (HANDOFF2 §3.2). Null in eid mode.
    std::unique_ptr<TrackManager> tracks;

    explicit Impl(const ConsensusConfig& c) : cfg(c) {
        if (cfg.stitch)
            tracks = std::make_unique<TrackManager>(6.0, cfg.lost_after_s);
    }

    TopicState& topic(uint8_t t) {
        auto it = topics.find(t);
        if (it != topics.end()) return it->second;
        TopicState st;
        // uninformed prior (§3.1)
        const double s = cfg.sigma_uninformed;
        st.prior_eta = 0.5 / (s * s);
        st.prior_lam = 1.0 / (s * s);
        return topics.emplace(t, st).first->second;
    }

    // belief (eta, lam) for a topic at time t: prior × live neighbour messages.
    std::pair<double, double> belief(const TopicState& st, double t) const {
        double eta = st.prior_eta, lam = st.prior_lam;
        for (const auto& kv : st.in) {
            const auto& [e, l, tt] = kv.second;
            if (t - tt <= cfg.lost_after_s) { eta += e; lam += l; }
        }
        return {eta, lam};
    }
};

AlertConsensus::AlertConsensus(const ConsensusConfig& cfg)
    : p_(std::make_unique<Impl>(cfg)) {}
AlertConsensus::~AlertConsensus() = default;
AlertConsensus::AlertConsensus(AlertConsensus&&) noexcept = default;
AlertConsensus& AlertConsensus::operator=(AlertConsensus&&) noexcept = default;

// §3.1 own observation → prior. Witness observing present/absent gets a tight
// prior at 1/0 (sigma_witness); an uninformed participant a weak prior at 0.5.
void AlertConsensus::observe(uint8_t topic, bool decision, bool witness) {
    TopicState& st = p_->topic(topic);
    st.witness = witness;
    double mu0, sig;
    if (witness) {
        mu0 = decision ? 1.0 : 0.0;
        sig = p_->cfg.sigma_witness;
    } else {
        mu0 = 0.5;
        sig = p_->cfg.sigma_uninformed;
    }
    st.prior_lam = 1.0 / (sig * sig);
    st.prior_eta = mu0 * st.prior_lam;
}

// §3.3 incoming message + §3.4 broadcast recompute, one beacon round.
void AlertConsensus::step(double t, const std::vector<Beacon>& heard) {
    const ConsensusConfig& cfg = p_->cfg;

    // Neighbour keys (§3.2): raw EID, or the persistent track when stitching.
    std::map<std::string, std::string> keys;
    if (p_->tracks) {
        auto tk = p_->tracks->update(t, heard);
        for (const auto& kv : tk) keys[kv.first] = "t" + std::to_string(kv.second);
    } else {
        for (const auto& b : heard) keys[b.eid] = b.eid;
    }

    for (const auto& b : heard) {
        const std::string& key = keys[b.eid];
        for (const auto& a : b.alerts) {
            TopicState& st = p_->topic(a.topic);
            // s_eta = a.lam·a.mu (info form of the sender's belief)
            auto [m_eta, m_lam] =
                through_smoothness(a.lam * a.mu, a.lam, cfg.sigma_consensus);
            const double cap = a.witness ? cfg.cap_witness : cfg.cap_hearsay;
            if (m_lam > cap) { m_eta *= cap / m_lam; m_lam = cap; }
            // damping EMA against this neighbour's previous message (§3.3). First
            // sighting: no EMA, store as-is.
            auto it = st.in.find(key);
            if (it != st.in.end()) {
                const double g = cfg.damping;
                m_eta = g * m_eta + (1 - g) * std::get<0>(it->second);
                m_lam = g * m_lam + (1 - g) * std::get<1>(it->second);
            }
            st.in[key] = {m_eta, m_lam, t};
        }
    }

    // prune long-dead entries so the store can't grow without bound (§3.4)
    for (auto& kv : p_->topics) {
        auto& in = kv.second.in;
        for (auto it = in.begin(); it != in.end();) {
            if (t - std::get<2>(it->second) > 4 * cfg.lost_after_s)
                it = in.erase(it);
            else
                ++it;
        }
    }
}

AlertState AlertConsensus::state(uint8_t topic, double t) const {
    AlertState s;
    s.topic = topic;
    auto it = p_->topics.find(topic);
    if (it == p_->topics.end()) return s;   // uninformed default (0.5, undecided)
    auto [eta, lam] = p_->belief(it->second, t);
    s.mu = lam > 1e-12 ? eta / lam : 0.5;
    s.lam = lam;
    s.decision = s.mu > 0.5;
    s.witness = it->second.witness;
    return s;
}

std::vector<AlertBelief> AlertConsensus::broadcast(double t) const {
    std::vector<AlertBelief> out;
    out.reserve(p_->topics.size());
    for (const auto& kv : p_->topics) {
        auto [eta, lam] = p_->belief(kv.second, t);
        AlertBelief a;
        a.topic = kv.first;
        a.mu = lam > 1e-12 ? eta / lam : 0.5;
        a.lam = lam;
        a.witness = kv.second.witness;
        out.push_back(a);
    }
    return out;
}

}  // namespace ara::osi::v2v
