// Unit parity test for the cooperative-alert consensus (impl/v2v/consensus.*).
//
// Port-correctness gate against tts/mesh_topo/consensus.py + HANDOFF2 §7. A
// fully-connected round-robin mesh (every vehicle hears every other's last
// broadcast) is the simplest faithful medium; the RF probe suite runs the churn/
// rotation/loss experiments (E1/E6/E7). Here we pin the LOAD-BEARING properties:
//   - convergence: uninformed vehicles adopt the witnessed decision;
//   - E3 majority: 4-present vs 2-absent → the network decides present;
//   - E5 containment: ONE witness cannot overturn an established opposite
//     consensus (§5.5 — the spoof/fault guard), but TWO concurring can.
// Scalar, deterministic, no radios.

#include "algo/beacon.hpp"
#include "algo/consensus.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace ara::osi::v2v;

namespace {

// A fully-connected fleet of N consensus nodes. Each round: every node emits its
// broadcast beliefs on a beacon; every node then hears all the OTHER beacons.
struct Fleet {
    std::vector<AlertConsensus> nodes;
    std::vector<std::vector<AlertBelief>> last;   // last broadcast per node

    explicit Fleet(int n, const ConsensusConfig& cfg = {}) {
        for (int i = 0; i < n; ++i) nodes.emplace_back(cfg);
        last.resize(n);
    }

    // Run `rounds` beacon rounds at dt spacing from t0.
    void run(int rounds, double t0 = 0.0, double dt = 5.0) {
        for (int r = 0; r < rounds; ++r) {
            const double t = t0 + r * dt;
            // 1) collect each node's outgoing beacon (its broadcast beliefs)
            std::vector<Beacon> beacons(nodes.size());
            for (size_t i = 0; i < nodes.size(); ++i) {
                beacons[i].t = t;
                beacons[i].eid = "v" + std::to_string(i);
                beacons[i].alerts = nodes[i].broadcast(t);
            }
            // 2) every node hears all the OTHERS
            for (size_t i = 0; i < nodes.size(); ++i) {
                std::vector<Beacon> heard;
                for (size_t j = 0; j < nodes.size(); ++j)
                    if (j != i) heard.push_back(beacons[j]);
                nodes[i].step(t, heard);
            }
        }
    }

    int decided_true(uint8_t topic, double t) const {
        int c = 0;
        for (const auto& n : nodes)
            if (n.state(topic, t).decision) ++c;
        return c;
    }
};

int fails = 0;
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
}

}  // namespace

int main() {
    const uint8_t TOPIC = 0;

    // ── Convergence: 1 witness (present) + 9 uninformed → all decide present ──
    {
        Fleet f(10);
        f.nodes[0].observe(TOPIC, /*present=*/true, /*witness=*/true);
        f.run(8);
        const double t = 8 * 5.0;
        check(f.decided_true(TOPIC, t) == 10,
              "converge: 1 witness present → all 10 decide present");
    }

    // ── E3 majority: 4 present vs 2 absent → network decides PRESENT ──────────
    {
        Fleet f(12);
        for (int i = 0; i < 4; ++i) f.nodes[i].observe(TOPIC, true, true);   // present
        for (int i = 4; i < 6; ++i) f.nodes[i].observe(TOPIC, false, true);  // absent
        f.run(12);
        const double t = 12 * 5.0;
        // the 6 uninformed vehicles should follow the majority (present)
        int uninformed_true = 0;
        for (int i = 6; i < 12; ++i)
            if (f.nodes[i].state(TOPIC, t).decision) ++uninformed_true;
        check(uninformed_true >= 5, "E3: 4-present vs 2-absent → majority present wins");
    }

    // ── E5 containment: a LONE witness cannot overturn an ESTABLISHED opposite
    //    consensus (§5.5). Establish 'present' with 3 witnesses, then a single
    //    'absent' witness must NOT flip the fleet. ─────────────────────────────
    {
        Fleet f(12);
        for (int i = 0; i < 3; ++i) f.nodes[i].observe(TOPIC, true, true);   // 3 present
        f.run(10);                                                            // establish
        // one vehicle now becomes an 'absent' witness (spoof/fault)
        f.nodes[3].observe(TOPIC, false, true);
        f.run(10, 10 * 5.0);
        const double t = 20 * 5.0;
        // the fleet must STILL be present (a lone opposite witness is contained)
        check(f.decided_true(TOPIC, t) >= 10,
              "E5: lone opposite witness cannot flip an established consensus");
    }

    // ── Two concurring witnesses CAN establish against an empty network ───────
    {
        Fleet f(12);
        f.nodes[0].observe(TOPIC, true, true);
        f.nodes[1].observe(TOPIC, true, true);
        f.run(10);
        const double t = 10 * 5.0;
        check(f.decided_true(TOPIC, t) == 12,
              "two concurring witnesses converge the fleet");
    }

    std::printf(fails ? "\nCONSENSUS PARITY: %d FAIL\n" : "\nCONSENSUS PARITY: all PASS\n",
                fails);
    return fails ? 1 : 0;
}
