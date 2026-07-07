// V2V SLAM — factors for the relative-topology graph.
//
// Port of mesh_topo/estimator/factors.py. Every factor exposes:
//   keys()                  — the variable keys it touches
//   residual(get)           — whitened residual r (so cost = 0.5||r||^2)
//   jacobian(get)           — d r / d x_key blocks, SAME row order as residual
// `get(key)` returns the current 2D estimate for a variable. Whitening folds the
// 1/sigma in so the graph just sums squared residuals. (HANDOFF §5.)
//
// Eigen-only, header-only. A "variable key" is a (track, frame) pair; a residual
// block's Jacobian is a (rows × 2) matrix w.r.t. that variable's 2D point.

#pragma once

#include <cmath>
#include <functional>
#include <utility>
#include <vector>

#include <Eigen/Dense>

namespace ara::osi::v2v {

// (track_id, frame_index) — the key for one 2D position variable.
using Key = std::pair<int, int>;

// Accessor: current 2D estimate for a variable (mirrors Python `get(key)`).
using Getter = std::function<Eigen::Vector2d(const Key&)>;

// One Jacobian block: the variable key + d(residual)/d(that point) (rows × 2).
struct JacBlock {
    Key                                       key;
    Eigen::Matrix<double, Eigen::Dynamic, 2>  J;
};

// Common factor interface — small virtual surface, the graph iterates over it.
struct Factor {
    virtual ~Factor() = default;
    virtual std::vector<Key>        keys() const = 0;
    virtual Eigen::VectorXd         residual(const Getter& get) const = 0;
    virtual std::vector<JacBlock>   jacobian(const Getter& get) const = 0;
};

// Soft prior pulling a variable toward a fixed 2D target. residual=(x-target)*w.
struct PriorFactor : Factor {
    Key             key;
    Eigen::Vector2d target;
    double          w;   // 1/sigma

    PriorFactor(Key k, Eigen::Vector2d t, double sigma)
        : key(std::move(k)), target(std::move(t)), w(1.0 / sigma) {}

    std::vector<Key> keys() const override { return {key}; }

    Eigen::VectorXd residual(const Getter& get) const override {
        return (get(key) - target) * w;
    }

    std::vector<JacBlock> jacobian(const Getter&) const override {
        Eigen::Matrix<double, Eigen::Dynamic, 2> J(2, 2);
        J = Eigen::Matrix2d::Identity() * w;
        return {{key, J}};
    }
};

// Between-factor: (x_b - x_a) should equal a known displacement `delta`.
struct OdometryFactor : Factor {
    Key             a, b;
    Eigen::Vector2d delta;
    double          w;

    OdometryFactor(Key ka, Key kb, Eigen::Vector2d d, double sigma)
        : a(std::move(ka)), b(std::move(kb)), delta(std::move(d)), w(1.0 / sigma) {}

    std::vector<Key> keys() const override { return {a, b}; }

    Eigen::VectorXd residual(const Getter& get) const override {
        return ((get(b) - get(a)) - delta) * w;
    }

    std::vector<JacBlock> jacobian(const Getter&) const override {
        Eigen::Matrix<double, Eigen::Dynamic, 2> Ja(2, 2), Jb(2, 2);
        Ja = -Eigen::Matrix2d::Identity() * w;
        Jb =  Eigen::Matrix2d::Identity() * w;
        return {{a, Ja}, {b, Jb}};
    }
};

// RSSI range factor evaluated in dB space (never inverts RSSI to a point).
//   residual r = (predicted_rssi(d) - measured_rssi) / sigma_rssi, scalar.
//   predicted_rssi(d) = A - 10 n log10(d).
//   d r / d x_i = (-10 n / ln10) * (x_i - x_j) / d^2 / sigma_rssi.
struct RangeRSSIFactor : Factor {
    Key    i, j;
    double rssi, A, n, w;   // w = 1/sigma_rssi

    RangeRSSIFactor(Key ki, Key kj, double rssi_, double A_, double n_,
                    double sigma_rssi)
        : i(std::move(ki)), j(std::move(kj)),
          rssi(rssi_), A(A_), n(n_), w(1.0 / sigma_rssi) {}

    std::vector<Key> keys() const override { return {i, j}; }

    // d (clamped at 1.0) + diff = x_i - x_j.
    void d_diff(const Getter& get, double& d, Eigen::Vector2d& diff) const {
        diff = get(i) - get(j);
        d = diff.norm();
        if (d < 1.0) d = 1.0;
    }

    Eigen::VectorXd residual(const Getter& get) const override {
        double d; Eigen::Vector2d diff;
        d_diff(get, d, diff);
        double pred = A - 10.0 * n * std::log10(d);
        Eigen::VectorXd r(1);
        r(0) = (pred - rssi) * w;
        return r;
    }

    std::vector<JacBlock> jacobian(const Getter& get) const override {
        double d; Eigen::Vector2d diff;
        d_diff(get, d, diff);
        Eigen::Vector2d g =
            (-10.0 * n / std::log(10.0)) * diff / (d * d) * w;
        Eigen::Matrix<double, Eigen::Dynamic, 2> Ji(1, 2), Jj(1, 2);
        Ji.row(0) = g.transpose();
        Jj.row(0) = (-g).transpose();
        return {{i, Ji}, {j, Jj}};
    }
};

}  // namespace ara::osi::v2v
