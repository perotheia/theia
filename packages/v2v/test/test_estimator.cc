// Unit parity test for the V2V relative-topology SLAM estimator (impl/v2v/).
//
// Port-correctness gate. Synthesizes beacons from KNOWN ground-truth
// constellations via the path-loss model (the simulator's RSSI = A − 10 n
// log10(d) + noise), runs the estimator, and asserts it recovers the RELATIVE
// topology (pairwise inter-vehicle distances — the gauge-free invariant) within
// tolerance. Covers the tts/HANDOFF_CPP_V2V.md operating point: window=10, K=6,
// short window, high SNR. Three geometries mirror the reference scenarios
// (chain / cluster), and the offset-solve restart determinism (fixed seed 0) is
// checked by re-running and asserting an identical result.

#include "algo/beacon.hpp"
#include "algo/estimator.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace ara::osi::v2v;

namespace {

double dist(const Vec2& a, const Vec2& b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}

// Build a window of beacons from a moving rigid constellation. `gt0` = initial
// positions; all move at (speed, heading) (a shared global frame). RSSI from the
// path-loss model + gaussian noise (sigma_db). Returns frames + times + the
// last-frame ground-truth positions (for the relative-topology check).
struct Window {
    std::vector<std::vector<Beacon>> frames;
    std::vector<double>              times;
    std::vector<Vec2>                last_gt;
};

Window make_window(const std::vector<Vec2>& gt0, double speed_mps,
                   double heading_deg, int F, double dt, double sigma_db,
                   uint32_t seed, double A = -40.0, double n = 2.8) {
    const int N = static_cast<int>(gt0.size());
    const double h = heading_deg * 3.14159265358979323846 / 180.0;
    const double vx = speed_mps * std::cos(h), vy = speed_mps * std::sin(h);
    std::mt19937 rng(seed);
    std::normal_distribution<double> noise(0.0, sigma_db);
    Window w;
    w.last_gt.resize(N);
    for (int f = 0; f < F; ++f) {
        const double t = f * dt;
        w.times.push_back(t);
        std::vector<Vec2> pos(N);
        for (int i = 0; i < N; ++i) pos[i] = {gt0[i].x + vx * t, gt0[i].y + vy * t};
        if (f == F - 1) w.last_gt = pos;
        std::vector<Beacon> fr;
        for (int i = 0; i < N; ++i) {
            Beacon b;
            b.t = t; b.eid = "E" + std::to_string(i); b.seq = static_cast<uint64_t>(f);
            b.heading_deg = heading_deg; b.speed_mps = speed_mps; b.true_id = i;
            for (int j = 0; j < N; ++j) {
                if (i == j) continue;
                double d = dist(pos[i], pos[j]); if (d < 1.0) d = 1.0;
                b.neighbors.push_back({"E" + std::to_string(j),
                                       A - 10.0 * n * std::log10(d) + noise(rng)});
            }
            fr.push_back(std::move(b));
        }
        w.frames.push_back(std::move(fr));
    }
    return w;
}

// RMS error of the recovered pairwise distances vs ground truth (gauge-free).
double pairwise_rmse(const EstimateResult& res, const std::vector<Vec2>& gt) {
    const int N = static_cast<int>(gt.size());
    double se = 0.0; int np = 0;
    for (int i = 0; i < N; ++i) {
        for (int j = i + 1; j < N; ++j) {
            // track id == true_id here (rotation off → identity association).
            if (!res.positions.count(i) || !res.positions.count(j)) continue;
            const double de = dist(res.positions.at(i), res.positions.at(j));
            const double dg = dist(gt[i], gt[j]);
            se += (de - dg) * (de - dg); ++np;
        }
    }
    return np ? std::sqrt(se / np) : 1e9;
}

}  // namespace

int main() {
    // ── 1. Highway chain (near-1D): 6 cars in a line, slight lane offset, 20 m/s
    //    east. The reference recovers longitudinal spacing essentially perfectly.
    {
        std::vector<Vec2> gt0;
        for (int i = 0; i < 6; ++i) gt0.push_back({i * 30.0, (i % 2) * 6.0});
        auto w = make_window(gt0, /*speed=*/20.0, /*heading=*/0.0,
                             /*F=*/10, /*dt=*/5.0, /*sigma_db=*/1.0, /*seed=*/1);
        TopologyEstimator est;   // tuned defaults (A=-40,n=2.8,...)
        auto res = est.estimate(w.frames, w.times);
        assert(res.positions.size() == gt0.size() && "all 6 chain tracks recovered");
        const double rmse = pairwise_rmse(res, w.last_gt);
        std::printf("[chain]   tracks=%zu pairwise-RMSE=%.2f m cost=%.2f\n",
                    res.positions.size(), rmse, res.cost);
        assert(rmse < 20.0 && "chain relative topology within tolerance");
        for (const auto& kv : res.positions)
            assert(std::isfinite(kv.second.x) && std::isfinite(kv.second.y));
    }

    // ── 2. Intersection cluster (2D, well-constrained): 7 cars around a crossing.
    //    The reference's BEST case (dense, rigid graph → ~5-9 m).
    {
        std::vector<Vec2> gt0 = {
            {0, 0}, {40, 5}, {-35, 10}, {10, 45}, {-15, -40}, {50, -30}, {-45, -25},
        };
        auto w = make_window(gt0, /*speed=*/15.0, /*heading=*/30.0,
                             /*F=*/10, /*dt=*/5.0, /*sigma_db=*/1.0, /*seed=*/2);
        TopologyEstimator est;
        auto res = est.estimate(w.frames, w.times);
        assert(res.positions.size() == gt0.size());
        const double rmse = pairwise_rmse(res, w.last_gt);
        std::printf("[cluster] tracks=%zu pairwise-RMSE=%.2f m cost=%.2f\n",
                    res.positions.size(), rmse, res.cost);
        assert(rmse < 30.0 && "cluster relative topology within tolerance");
    }

    // ── 3. Determinism: the fixed-seed (0) restart structure must be reproducible
    //    — re-running the SAME window yields an identical solution (HANDOFF §3.3).
    {
        std::vector<Vec2> gt0 = {{0, 0}, {30, 4}, {60, -2}, {90, 6}};
        auto w = make_window(gt0, 22.0, 10.0, 8, 5.0, 1.2, /*seed=*/3);
        TopologyEstimator e1, e2;
        auto r1 = e1.estimate(w.frames, w.times);
        auto r2 = e2.estimate(w.frames, w.times);
        assert(r1.positions.size() == r2.positions.size());
        for (const auto& kv : r1.positions) {
            const Vec2& a = kv.second;
            const Vec2& b = r2.positions.at(kv.first);
            assert(std::abs(a.x - b.x) < 1e-9 && std::abs(a.y - b.y) < 1e-9 &&
                   "fixed-seed restart → deterministic solve");
        }
        std::printf("[determinism] identical re-solve over %zu tracks\n",
                    r1.positions.size());
    }

    std::printf("ALL ESTIMATOR PARITY CHECKS PASSED\n");
    return 0;
}
