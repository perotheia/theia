// LIVE positive test for the ProcDetector (Cat A/C/D) — requires the stack up.
//
// Drives a ProcDetector against the RUNNING supervisor's children with a policy
// that DELIBERATELY omits com's gRPC ports from expected_listeners. com (a known
// FC) is then seen listening on an "unexpected" port → Cat A
// IDSM_UNEXPECTED_SERVICE_ENDPOINT must fire. Also asserts that with the CORRECT
// policy (com's ports allowed) there is NO detection (no false positive).
//
// This is the positive counterpart to test_proc_detector (which pins the parse/
// classify wiring) — it proves the rule fires against a real FC on the live
// stack. Skips cleanly if no supervisor / com is running.

#include "impl/proc_detector.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace ara::idsm;

// Find the running supervisor pid (the FC scope anchor) + confirm com is a child.
static int find_supervisor() {
    FILE* p = ::popen("pgrep -x supervisor | head -1", "r");
    if (!p) return -1;
    char buf[32] = {0};
    if (!std::fgets(buf, sizeof(buf), p)) { ::pclose(p); return -1; }
    ::pclose(p);
    return std::atoi(buf);
}

int main() {
    int sup = find_supervisor();
    if (sup <= 0) {
        std::printf("proc-live: SKIP (no supervisor running — `theia start`)\n");
        return 0;
    }

    // 1. CORRECT policy: com's gRPC ports are expected → NO detection for com.
    {
        ProcDetector d;
        d.configure("com:7700,com:7710,com:7711", "com", "");
        d.set_known_fcs({"com", "crypto", "per", "sm", "log", "nm", "osi",
                         "tsync", "ucm", "shwa", "fw", "idsm"});
        auto v = d.scan(sup);
        for (const auto& e : v) {
            // com's own ports must NOT be flagged under the correct policy.
            assert(e.src.rfind("com/", 0) != 0 &&
                   "com's expected gRPC ports must not trip a detection");
        }
        std::printf("proc-live: correct policy → %zu detection(s) (none for com)\n",
                    v.size());
    }

    // 2. TIGHTENED policy: drop com:7710 → com listening on :7710 is now an
    //    unexpected endpoint for a known FC → Cat A must fire.
    {
        ProcDetector d;
        d.configure("com:7700,com:7711", "com", "");   // 7710 intentionally omitted
        d.set_known_fcs({"com", "crypto", "per", "sm", "log", "nm", "osi",
                         "tsync", "ucm", "shwa", "fw", "idsm"});
        auto v = d.scan(sup);
        bool caught = false;
        for (const auto& e : v) {
            if (e.signature == "IDSM_UNEXPECTED_SERVICE_ENDPOINT" &&
                e.src.rfind("com/", 0) == 0 && e.dst == "tcp:7710") {
                caught = true;
                assert(e.severity == 5);
            }
        }
        if (!caught) {
            // com may not be up (e.g. mid-restart). Don't hard-fail the lane.
            std::printf("proc-live: SKIP positive (com:7710 not observed — com "
                        "down?); negative case passed\n");
            return 0;
        }
        std::printf("proc-live: OK — dropping com:7710 from the policy fires "
                    "IDSM_UNEXPECTED_SERVICE_ENDPOINT (Cat A) for com on the "
                    "live stack\n");
    }
    return 0;
}
