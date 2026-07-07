// V2V SLAM — sparse Gauss-Newton / Levenberg-Marquardt with a Huber kernel.
//
// Port of mesh_topo/estimator/graph.py::FactorGraph. Variables are 2D points
// keyed by (track, frame). Each factor supplies a whitened residual + Jacobian
// blocks (factors.hpp). We assemble the normal equations (J^T W J) dx = -J^T W r
// with per-residual Huber weights and take damped steps, accepting only when the
// total robust cost decreases. Dense solve (problems are small: ≤64 tracks ×
// window ≤ 12 frames). (HANDOFF §5 / §8.)

#pragma once

#include <map>
#include <memory>
#include <vector>

#include <Eigen/Dense>

#include "factors.hpp"

namespace ara::osi::v2v {

class FactorGraph {
public:
    // Register a variable + its initial value (idempotent on the key).
    void add_variable(const Key& key, const Eigen::Vector2d& init);

    // Add a factor; any touched-but-unseen variable is created at the origin.
    void add_factor(std::shared_ptr<Factor> f);

    // Solve. Returns (solution map key->2D, final robust cost).
    std::pair<std::map<Key, Eigen::Vector2d>, double>
    solve(double huber_delta = 6.0, int iters = 30);

private:
    double robust_cost(const Eigen::VectorXd& x, double delta) const;

    std::vector<Key>                       keys_;
    std::map<Key, int>                     index_;
    std::map<Key, Eigen::Vector2d>         init_;
    std::vector<std::shared_ptr<Factor>>   factors_;
};

}  // namespace ara::osi::v2v
