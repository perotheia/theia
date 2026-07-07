// Port of mesh_topo/estimator/graph.py::FactorGraph (dense Huber LM).

#include "graph.hpp"

#include <cmath>

namespace ara::osi::v2v {

namespace {
// Huber reweight (graph.py::_huber_weight): 1 inside the band, sqrt(delta/r_norm)
// outside. r_norm==0 → 1 (no division).
double huber_weight(double r_norm, double delta) {
    if (r_norm <= delta || r_norm == 0.0) return 1.0;
    return std::sqrt(delta / r_norm);
}
}  // namespace

void FactorGraph::add_variable(const Key& key, const Eigen::Vector2d& init) {
    if (index_.find(key) == index_.end()) {
        index_[key] = static_cast<int>(keys_.size());
        keys_.push_back(key);
    }
    init_[key] = init;
}

void FactorGraph::add_factor(std::shared_ptr<Factor> f) {
    for (const Key& k : f->keys())
        if (index_.find(k) == index_.end())
            add_variable(k, Eigen::Vector2d::Zero());
    factors_.push_back(std::move(f));
}

// Total robust cost (graph.py::_robust_cost): 0.5||r||^2 inside the band, the
// Huber linear tail delta*(||r|| - 0.5 delta) outside.
double FactorGraph::robust_cost(const Eigen::VectorXd& x, double delta) const {
    auto get = [&](const Key& k) -> Eigen::Vector2d {
        const int i = index_.at(k);
        return x.segment<2>(2 * i);
    };
    double c = 0.0;
    for (const auto& fac : factors_) {
        const Eigen::VectorXd r = fac->residual(get);
        const double rn = r.norm();
        if (rn <= delta) c += 0.5 * rn * rn;
        else             c += delta * (rn - 0.5 * delta);
    }
    return c;
}

std::pair<std::map<Key, Eigen::Vector2d>, double>
FactorGraph::solve(double huber_delta, int iters) {
    const int N = static_cast<int>(keys_.size());
    Eigen::VectorXd x = Eigen::VectorXd::Zero(2 * N);
    for (const auto& kv : index_)
        x.segment<2>(2 * kv.second) = init_.at(kv.first);

    auto get_from = [&](const Eigen::VectorXd& vec) {
        return [&vec, this](const Key& k) -> Eigen::Vector2d {
            return vec.segment<2>(2 * index_.at(k));
        };
    };

    double lam = 1e-3;
    double cost = robust_cost(x, huber_delta);

    for (int it = 0; it < iters; ++it) {
        Eigen::MatrixXd H = Eigen::MatrixXd::Zero(2 * N, 2 * N);
        Eigen::VectorXd g = Eigen::VectorXd::Zero(2 * N);
        auto get = get_from(x);

        for (const auto& fac : factors_) {
            const Eigen::VectorXd r = fac->residual(get);
            const double rn = r.norm();
            const double w = huber_weight(rn, huber_delta);
            const std::vector<JacBlock> blocks = fac->jacobian(get);
            for (const auto& ba : blocks) {
                const int ia = index_.at(ba.key);
                g.segment<2>(2 * ia) += w * (ba.J.transpose() * r);
                for (const auto& bb : blocks) {
                    const int ib = index_.at(bb.key);
                    H.block<2, 2>(2 * ia, 2 * ib) +=
                        w * (ba.J.transpose() * bb.J);
                }
            }
        }

        bool improved = false;
        for (int t = 0; t < 10; ++t) {
            // H + lam * diag(diag(H) + 1e-9), solved via LDLT (SPD normal eqs).
            Eigen::MatrixXd A = H;
            for (int d = 0; d < 2 * N; ++d)
                A(d, d) += lam * (H(d, d) + 1e-9);
            Eigen::LDLT<Eigen::MatrixXd> ldlt(A);
            if (ldlt.info() != Eigen::Success) { lam *= 10; continue; }
            const Eigen::VectorXd dx = ldlt.solve(-g);
            if (!dx.allFinite()) { lam *= 10; continue; }
            const Eigen::VectorXd xc = x + dx;
            const double cc = robust_cost(xc, huber_delta);
            if (cc < cost) {
                x = xc; cost = cc; lam = std::max(lam * 0.5, 1e-12);
                improved = true;
                break;
            }
            lam *= 10;
        }
        if (!improved) break;
    }

    std::map<Key, Eigen::Vector2d> sol;
    for (const Key& k : keys_)
        sol[k] = x.segment<2>(2 * index_.at(k));
    return {sol, cost};
}

}  // namespace ara::osi::v2v
