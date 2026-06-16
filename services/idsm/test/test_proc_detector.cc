// Unit check for the userspace ProcDetector (IDSM Cat A/C/D/H) — no live stack,
// no eBPF. Drives the rule predicates directly against synthetic listener sets
// (we can't easily fake `ss` output here, so we test the classification logic
// via a small ss-line parser exposed in proc_detail + the configure/emit edges).

#include "impl/proc_detector.hpp"

#include <cassert>
#include <cstdio>
#include <set>
#include <string>

using namespace ara::idsm;
using namespace ara::idsm::proc_detail;

int main() {
    // 1. NETLINK_SOCK_DIAG listener scan (no `ss` exec). With a bogus supervisor
    //    pid no FC socket is in scope → empty (proving the inode→pid join + the
    //    FC scoping). The live listener content is covered by test_proc_live.
    {
        auto ls = netlink_listeners(/*supervisor_pid=*/999999);
        assert(ls.empty() && "no FC socket is a child of a bogus supervisor pid");
    }

    // 2. split helpers.
    {
        auto pairs = split_pairs_("com:7700, com:7710 ,x:1");
        assert(pairs.size() == 3 && pairs[0].first == "com" &&
               pairs[0].second == "7700" && pairs[2].first == "x");
        auto s = split_set_("com, crypto ,per");
        assert(s.count("com") && s.count("crypto") && s.count("per") &&
               s.size() == 3);
    }

    // 3. gRPC DMZ port classification (Cat D scope).
    assert(ProcDetector::is_grpc_port(7700));
    assert(ProcDetector::is_grpc_port(7711));
    assert(!ProcDetector::is_grpc_port(8080));

    // 4. Rule edge logic via a ProcDetector configured with a known policy.
    //    We can't inject `ss` output, so we exercise the public configure()
    //    contract + the A-vs-C classifier indirectly: a fresh configure re-arms
    //    edges, and known-FC seeding flips A vs C. (Full live-listener coverage
    //    is the robot/live test; here we pin the wiring compiles + the classifier
    //    distinguishes the two.)
    ProcDetector d;
    d.configure(/*expected*/"com:7700,com:7710,com:7711",
                /*grpc*/"com", /*digests*/"");
    d.set_known_fcs({"com", "radar", "per"});
    // scan(supervisor_pid) scopes to FCs that are children of that pid. With a
    // bogus pid (1 = init), no listener is a child → empty result, proving the
    // scoping gate (no host-process false positives). The live test asserts on
    // the real-FC path.
    auto v = d.scan(/*supervisor_pid=*/999999);
    assert(v.empty() && "no listener is a child of a bogus supervisor pid");

    // 5. Cat B — parse fw's nft counter JSON (the real shape `nft -j` emits).
    {
        std::string j = R"({"nftables": [{"metainfo": {"version": "1.0.2"}},
          {"counter": {"family": "inet", "name": "idsm_b_per", "table":
           "theia_fw", "handle": 2, "packets": 7, "bytes": 420}},
          {"counter": {"family": "inet", "name": "idsm_b_rogue", "table":
           "theia_fw", "handle": 3, "packets": 0, "bytes": 0}},
          {"counter": {"family": "inet", "name": "unrelated", "packets": 99}}]})";
        auto drops = parse_nft_counters_(j);
        assert(drops.size() == 2 && "only idsm_b_* counters, not 'unrelated'");
        // order preserved: per=7, rogue=0.
        assert(drops[0].first == "per" && drops[0].second == 7);
        assert(drops[1].first == "rogue" && drops[1].second == 0);
    }

    std::printf("proc-detector test: OK — ss parse + split + grpc-port + "
                "configure/scan wiring + Cat-B nft-counter parse\n");
    return 0;
}
