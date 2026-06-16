// Unit check for the per-FC egress ruleset generation (fw_backend.hpp).
//
// Asserts build_ruleset emits a valid `output` chain doing `socket cgroupv2
// level 2 "theia.slice/<fc>"` per-FC egress allow-list-then-drop, and that an FC
// whose cgroup ISN'T placed under the slice is skipped (graceful — nft would
// reject a rule naming a non-existent cgroup path). Creates a throwaway slice +
// sub-cgroup so fc_cgroup_exists() sees one FC. Run as root (cgroup mkdir).

#include "impl/fw_backend.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

int main() {
    const std::string root = "/tmp/idsm_cgtest";     // fake cgroup root (writable)
    const std::string slice = "theia.slice";
    // Place "per" but NOT "rogue" under the slice.
    ::mkdir(root.c_str(), 0755);
    ::mkdir((root + "/" + slice).c_str(), 0755);
    ::mkdir((root + "/" + slice + "/per").c_str(), 0755);
    { FILE* f = ::fopen((root + "/" + slice + "/per/cgroup.procs").c_str(), "w");
      if (f) ::fclose(f); }

    int rules = 0, ov = 0, eg = 0;
    std::string rs = ara::fw::build_ruleset(
        "7700,7710,7711,2379", "drop", "/nonexistent-fwd",
        "per=10.0.0.0/8,192.168.0.0/16;rogue=10.0.0.0/8",
        slice, root, rules, ov, eg);

    // per is placed → enforced; rogue is not → skipped.
    assert(eg == 1 && "only the placed FC (per) gets an egress rule");
    assert(rs.find("chain output {") != std::string::npos);
    assert(rs.find("socket cgroupv2 level 2 \"theia.slice/per\"") != std::string::npos);
    assert(rs.find("ip daddr { 10.0.0.0/8, 192.168.0.0/16 } accept") != std::string::npos);
    // denied egress: a named per-FC counter (idsm polls it for Cat B) + log + drop.
    assert(rs.find("counter idsm_b_per { }") != std::string::npos &&
           "the per-FC drop counter is declared at table scope");
    assert(rs.find("counter name \"idsm_b_per\" log prefix \"IDSM_B per \" drop")
           != std::string::npos &&
           "denied egress increments idsm_b_<fc> + logs + drops");
    assert(rs.find("theia.slice/rogue") == std::string::npos &&
           "an unplaced FC must not appear in the ruleset");
    // input chain still intact.
    assert(rs.find("tcp dport { 7700, 7710, 7711, 2379 } accept") != std::string::npos);

    std::printf("fw-egress-gen: OK — output chain enforces per-FC egress via "
                "socket cgroupv2; placed FC enforced, unplaced FC skipped; "
                "denied egress logged IDSM_B for idsm correlation\n");

    // cleanup
    ::unlink((root + "/" + slice + "/per/cgroup.procs").c_str());
    ::rmdir((root + "/" + slice + "/per").c_str());
    ::rmdir((root + "/" + slice).c_str());
    ::rmdir(root.c_str());
    return 0;
}
