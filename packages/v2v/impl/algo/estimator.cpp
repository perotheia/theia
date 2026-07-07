// Port of mesh_topo/estimator/estimator.py::TopologyEstimator (1:1 with the
// Python behavioural reference). The Eigen pipeline + the stateful TrackManager
// live HERE behind TopologyEstimator::Impl, so the public header stays Eigen-free.

#include "estimator.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <random>

#include <Eigen/Dense>

#include "association.hpp"   // TrackManager, beacon_vel
#include "factors.hpp"       // Key, the 3 factors
#include "graph.hpp"         // FactorGraph

namespace ara::osi::v2v {

namespace {
constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

Eigen::Vector2d bvel(const Beacon& b) {
    const double h = b.heading_deg * kDegToRad;
    return b.speed_mps * Eigen::Vector2d(std::cos(h), std::sin(h));
}

Vec2 to_vec2(const Eigen::Vector2d& v) { return Vec2{v.x(), v.y()}; }

int most_observed(const std::map<int, std::vector<int>>& present) {
    int best = -1; size_t best_n = 0;
    for (const auto& kv : present)
        if (kv.second.size() > best_n) { best_n = kv.second.size(); best = kv.first; }
    return best;
}
}  // namespace

// ── Impl: all Eigen + the stateful tracker ──────────────────────────────────
struct TopologyEstimator::Impl {
    EstConfig    cfg;
    TrackManager tracker;

    using RangeObs = struct { int ti, tj, f; double rssi; };
    using ShapeMap = std::map<int, std::map<int, Eigen::Vector2d>>;
    using OffMap   = std::map<int, Eigen::Vector2d>;

    explicit Impl(EstConfig c)
        : cfg(c), tracker(/*gate_vel=*/6.0, /*lost_after_s=*/3.0) {}

    ShapeMap odom_shapes(const std::map<int, std::vector<int>>& present,
                         const std::map<Key, Eigen::Vector2d>& kin_vel,
                         const std::vector<double>& times) const {
        ShapeMap shape;
        for (const auto& kv : present) {
            const int tr = kv.first;
            const std::vector<int>& fs = kv.second;
            std::map<int, Eigen::Vector2d> s;
            s[fs.front()] = Eigen::Vector2d::Zero();
            for (size_t i = 0; i + 1 < fs.size(); ++i) {
                const int a = fs[i], b = fs[i + 1];
                const double dt = times[b] - times[a];
                const Eigen::Vector2d v = 0.5 * (kin_vel.at({tr, a}) + kin_vel.at({tr, b}));
                s[b] = s[a] + v * dt;
            }
            shape[tr] = std::move(s);
        }
        return shape;
    }

    std::vector<RangeObs> collect_ranges(
            const std::vector<std::vector<Beacon>>& frames,
            const std::vector<std::map<std::string, int>>& assoc,
            const std::map<int, std::vector<int>>& present) const {
        std::vector<RangeObs> obs;
        auto present_has = [&](int tr, int f) {
            auto it = present.find(tr);
            if (it == present.end()) return false;
            return std::find(it->second.begin(), it->second.end(), f) != it->second.end();
        };
        for (size_t f = 0; f < frames.size(); ++f) {
            const auto& m = assoc[f];
            for (const auto& b : frames[f]) {
                const int ti = m.at(b.eid);
                for (const auto& ob : b.neighbors) {
                    auto mj = m.find(ob.neighbor_eid);
                    if (mj == m.end()) continue;
                    const int tj = mj->second;
                    if (ti != tj && present_has(ti, (int)f) && present_has(tj, (int)f))
                        obs.push_back({ti, tj, (int)f, ob.rssi});
                }
            }
        }
        return obs;
    }

    double range_cost_offsets(const ShapeMap& shape, const OffMap& offsets,
                              const std::vector<RangeObs>& range_obs) const {
        double c = 0.0;
        for (const auto& o : range_obs) {
            const Eigen::Vector2d pi = offsets.at(o.ti) + shape.at(o.ti).at(o.f);
            const Eigen::Vector2d pj = offsets.at(o.tj) + shape.at(o.tj).at(o.f);
            double d = (pi - pj).norm(); if (d < 1.0) d = 1.0;
            const double r = (cfg.A - 10.0 * cfg.n * std::log10(d) - o.rssi) / cfg.sigma_rssi;
            c += std::min(r * r, cfg.huber_delta * cfg.huber_delta);
        }
        return c;
    }

    OffMap solve_offsets(const std::map<int, std::vector<int>>& present,
                         const ShapeMap& shape,
                         const std::vector<RangeObs>& range_obs) const {
        std::vector<int> tracks;
        for (const auto& kv : present) tracks.push_back(kv.first);
        const int N = static_cast<int>(tracks.size());
        std::map<int, int> idx;
        for (int i = 0; i < N; ++i) idx[tracks[i]] = i;
        const int anchor = most_observed(present);
        const int ai = idx[anchor];

        auto pos = [&](const Eigen::MatrixX2d& off, int tr, int f) -> Eigen::Vector2d {
            return off.row(idx.at(tr)).transpose() + shape.at(tr).at(f);
        };
        auto cost_of = [&](const Eigen::MatrixX2d& off) {
            double c = 0.0;
            for (const auto& o : range_obs) {
                double d = (pos(off, o.ti, o.f) - pos(off, o.tj, o.f)).norm();
                if (d < 1.0) d = 1.0;
                const double r = (cfg.A - 10.0 * cfg.n * std::log10(d) - o.rssi) / cfg.sigma_rssi;
                c += std::min(r * r, cfg.huber_delta * cfg.huber_delta);
            }
            return c;
        };

        auto run = [&](Eigen::MatrixX2d off) -> std::pair<Eigen::MatrixX2d, double> {
            double lam = 1e-2;
            double c_cur = cost_of(off);
            for (int it = 0; it < 40; ++it) {
                Eigen::MatrixXd H = Eigen::MatrixXd::Zero(2 * N, 2 * N);
                Eigen::VectorXd g = Eigen::VectorXd::Zero(2 * N);
                for (const auto& o : range_obs) {
                    Eigen::Vector2d diff = pos(off, o.ti, o.f) - pos(off, o.tj, o.f);
                    double d = diff.norm(); if (d < 1.0) d = 1.0;
                    const double r = (cfg.A - 10.0 * cfg.n * std::log10(d) - o.rssi) / cfg.sigma_rssi;
                    const double w = (std::abs(r) <= cfg.huber_delta)
                                         ? 1.0 : std::sqrt(cfg.huber_delta / std::abs(r));
                    const Eigen::Vector2d J =
                        (-10.0 * cfg.n / std::log(10.0)) * diff / (d * d) / cfg.sigma_rssi;
                    const int ii = idx.at(o.ti), jj = idx.at(o.tj);
                    const std::pair<int, double> ab[2] = {{ii, 1.0}, {jj, -1.0}};
                    for (const auto& a : ab) {
                        g.segment<2>(2 * a.first) += w * a.second * J * r;
                        for (const auto& b : ab)
                            H.block<2, 2>(2 * a.first, 2 * b.first) +=
                                (w * w) * a.second * b.second * (J * J.transpose());
                    }
                }
                H.block<2, 2>(2 * ai, 2 * ai) +=
                    Eigen::Matrix2d::Identity() / (cfg.anchor_sigma * cfg.anchor_sigma);
                g.segment<2>(2 * ai) +=
                    off.row(ai).transpose() / (cfg.anchor_sigma * cfg.anchor_sigma);

                bool improved = false;
                for (int t = 0; t < 8; ++t) {
                    Eigen::MatrixXd Aa = H;
                    for (int dd = 0; dd < 2 * N; ++dd) Aa(dd, dd) += lam * (H(dd, dd) + 1e-6);
                    Eigen::LDLT<Eigen::MatrixXd> ldlt(Aa);
                    if (ldlt.info() != Eigen::Success) { lam *= 10; continue; }
                    const Eigen::VectorXd dx = ldlt.solve(-g);
                    if (!dx.allFinite()) { lam *= 10; continue; }
                    Eigen::MatrixX2d cand = off;
                    for (int k = 0; k < N; ++k) cand.row(k) += dx.segment<2>(2 * k).transpose();
                    const double c_new = cost_of(cand);
                    if (c_new < c_cur) {
                        off = cand; c_cur = c_new; lam = std::max(lam * 0.5, 1e-9);
                        improved = true; break;
                    }
                    lam *= 10;
                }
                if (!improved) break;
            }
            return {off, c_cur};
        };

        Eigen::MatrixX2d best_off;
        double best_c = std::numeric_limits<double>::infinity();
        std::mt19937 rng(0);
        std::normal_distribution<double> gauss(0.0, 120.0);
        std::vector<Eigen::MatrixX2d> starts;
        starts.push_back(Eigen::MatrixX2d::Zero(N, 2));
        for (int s = 0; s < 5; ++s) {
            Eigen::MatrixX2d m(N, 2);
            for (int r = 0; r < N; ++r) { m(r, 0) = gauss(rng); m(r, 1) = gauss(rng); }
            starts.push_back(m);
        }
        for (Eigen::MatrixX2d s0 : starts) {
            const Eigen::RowVector2d a0 = s0.row(ai);
            s0.rowwise() -= a0;
            auto res = run(s0);
            if (res.second < best_c) { best_c = res.second; best_off = res.first; }
        }

        OffMap out;
        for (int i = 0; i < N; ++i) out[tracks[i]] = best_off.row(i).transpose();
        return out;
    }

    OffMap resolve_reflection(const std::map<int, std::vector<int>>& present,
                              const ShapeMap& shape, const OffMap& offsets,
                              const std::vector<RangeObs>& range_obs) const {
        const int anchor = most_observed(present);
        const Eigen::Vector2d a_off = offsets.at(anchor);
        OffMap mirror;
        for (const auto& kv : offsets)
            mirror[kv.first] = Eigen::Vector2d(kv.second.x(), 2.0 * a_off.y() - kv.second.y());
        return (range_cost_offsets(shape, mirror, range_obs) <
                range_cost_offsets(shape, offsets, range_obs)) ? mirror : offsets;
    }

    EstimateResult estimate(const std::vector<std::vector<Beacon>>& frames,
                            const std::vector<double>& times) {
        const int F = static_cast<int>(frames.size());

        std::vector<std::map<std::string, int>> assoc;
        std::map<Key, Eigen::Vector2d> kin_vel;
        std::map<int, std::vector<int>> present;
        for (int f = 0; f < F; ++f) {
            auto m = tracker.update(times[f], frames[f]);
            assoc.push_back(m);
            for (const auto& b : frames[f]) {
                const int tr = m.at(b.eid);
                kin_vel[{tr, f}] = bvel(b);
                present[tr].push_back(f);
            }
        }
        if (present.empty()) return {};

        ShapeMap shape = odom_shapes(present, kin_vel, times);
        std::vector<RangeObs> range_obs = collect_ranges(frames, assoc, present);
        OffMap offsets = solve_offsets(present, shape, range_obs);
        offsets = resolve_reflection(present, shape, offsets, range_obs);

        FactorGraph g;
        for (const auto& kv : present) {
            const int tr = kv.first;
            for (int f : kv.second) {
                const Eigen::Vector2d init = offsets.at(tr) + shape.at(tr).at(f);
                g.add_variable({tr, f}, init);
                g.add_factor(std::make_shared<PriorFactor>(Key{tr, f}, init,
                                                           cfg.weak_prior_sigma));
            }
        }
        for (const auto& kv : present) {
            const int tr = kv.first;
            const std::vector<int>& fs = kv.second;
            for (size_t i = 0; i + 1 < fs.size(); ++i) {
                const int a = fs[i], b = fs[i + 1];
                const double dt = times[b] - times[a];
                const Eigen::Vector2d v = 0.5 * (kin_vel.at({tr, a}) + kin_vel.at({tr, b}));
                g.add_factor(std::make_shared<OdometryFactor>(
                    Key{tr, a}, Key{tr, b}, v * dt, cfg.sigma_odom * std::max(dt, 1.0)));
            }
        }
        for (int f = 0; f < F; ++f) {
            const auto& m = assoc[f];
            for (const auto& b : frames[f]) {
                const int ti = m.at(b.eid);
                for (const auto& ob : b.neighbors) {
                    auto mj = m.find(ob.neighbor_eid);
                    if (mj == m.end()) continue;
                    const int tj = mj->second;
                    if (ti == tj) continue;
                    if (kin_vel.find({ti, f}) == kin_vel.end() ||
                        kin_vel.find({tj, f}) == kin_vel.end()) continue;
                    g.add_factor(std::make_shared<RangeRSSIFactor>(
                        Key{ti, f}, Key{tj, f}, ob.rssi, cfg.A, cfg.n, cfg.sigma_rssi));
                }
            }
        }
        const int anchor = most_observed(present);
        const int af = present.at(anchor).front();
        g.add_factor(std::make_shared<PriorFactor>(
            Key{anchor, af}, Eigen::Vector2d::Zero(), cfg.anchor_sigma));

        auto solved = g.solve(cfg.huber_delta);
        std::map<Key, Eigen::Vector2d> sol = solved.first;
        const double cost = solved.second;

        auto range_cost_full = [&](bool use_full) {
            double c = 0.0;
            for (const auto& o : range_obs) {
                Eigen::Vector2d pi = use_full ? sol.at({o.ti, o.f})
                                              : (offsets.at(o.ti) + shape.at(o.ti).at(o.f));
                Eigen::Vector2d pj = use_full ? sol.at({o.tj, o.f})
                                              : (offsets.at(o.tj) + shape.at(o.tj).at(o.f));
                double d = (pi - pj).norm(); if (d < 1.0) d = 1.0;
                const double r = (cfg.A - 10.0 * cfg.n * std::log10(d) - o.rssi) / cfg.sigma_rssi;
                c += std::min(r * r, cfg.huber_delta * cfg.huber_delta);
            }
            return c;
        };
        if (range_cost_full(true) > range_cost_full(false) * 1.05) {
            sol.clear();
            for (const auto& kv : present)
                for (int f : kv.second)
                    sol[{kv.first, f}] = offsets.at(kv.first) + shape.at(kv.first).at(f);
        }

        EstimateResult out;
        out.anchor = anchor;
        out.cost = cost;
        for (const auto& kv : present) {
            const int tr = kv.first;
            const int lf = kv.second.back();
            out.positions[tr]  = to_vec2(sol.at({tr, lf}));
            out.velocities[tr] = to_vec2(kin_vel.at({tr, lf}));
        }
        return out;
    }
};

// ── public facade ───────────────────────────────────────────────────────────
TopologyEstimator::TopologyEstimator(EstConfig cfg)
    : impl_(std::make_unique<Impl>(cfg)) {}
TopologyEstimator::~TopologyEstimator() = default;
TopologyEstimator::TopologyEstimator(TopologyEstimator&&) noexcept = default;
TopologyEstimator& TopologyEstimator::operator=(TopologyEstimator&&) noexcept = default;

EstimateResult TopologyEstimator::estimate(
        const std::vector<std::vector<Beacon>>& frames,
        const std::vector<double>& times) {
    return impl_->estimate(frames, times);
}

const EstConfig& TopologyEstimator::config() const { return impl_->cfg; }

}  // namespace ara::osi::v2v
