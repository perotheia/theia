// Per-node CPU affinity + scheduler. See NodeAffinity.hh.

#include "NodeAffinity.hh"
#include "Logger.hh"   // process_logger() for soft-fail diagnostics

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace theia {
namespace runtime {

namespace {

struct NodeCfg {
    bool                has_cpu   = false;
    std::vector<int>    cpus;
    int                 policy    = -1;   // -1 = leave default; else SCHED_*
    int                 prio      = 0;    // rtprio (fifo/rr)
    bool                has_nice  = false;
    int                 nice      = 0;
    uint64_t            dl_runtime = 0, dl_deadline = 0, dl_period = 0;
};

int policy_of(const std::string& s) {
    if (s == "other")    return SCHED_OTHER;
    if (s == "batch")    return SCHED_BATCH;
    if (s == "idle")     return SCHED_IDLE;
    if (s == "fifo")     return SCHED_FIFO;
    if (s == "rr")       return SCHED_RR;
#ifdef SCHED_DEADLINE
    if (s == "deadline") return SCHED_DEADLINE;
#endif
    return -1;
}

// Split "a,b,c" → ints.
std::vector<int> parse_ints(const std::string& csv) {
    std::vector<int> out;
    size_t i = 0;
    while (i < csv.size()) {
        size_t j = csv.find(',', i);
        if (j == std::string::npos) j = csv.size();
        const std::string tok = csv.substr(i, j - i);
        if (!tok.empty()) {
            try { out.push_back(std::stoi(tok)); } catch (...) {}
        }
        i = j + 1;
    }
    return out;
}

// Pull this node's "<node>=<fields>" record out of the |-separated spec.
bool find_record(const char* spec, const char* node, std::string& fields) {
    if (!spec || !node) return false;
    const std::string s = spec;
    const std::string key = std::string(node) + "=";
    size_t i = 0;
    while (i < s.size()) {
        size_t bar = s.find('|', i);
        if (bar == std::string::npos) bar = s.size();
        const std::string rec = s.substr(i, bar - i);
        if (rec.rfind(key, 0) == 0) {   // starts with "<node>="
            fields = rec.substr(key.size());
            return true;
        }
        i = bar + 1;
    }
    return false;
}

// Parse ";"-separated "k:v" fields of a record into NodeCfg.
NodeCfg parse_fields(const std::string& fields) {
    NodeCfg c;
    size_t i = 0;
    while (i < fields.size()) {
        size_t semi = fields.find(';', i);
        if (semi == std::string::npos) semi = fields.size();
        const std::string f = fields.substr(i, semi - i);
        i = semi + 1;
        const size_t colon = f.find(':');
        if (colon == std::string::npos) continue;
        const std::string k = f.substr(0, colon);
        const std::string v = f.substr(colon + 1);
        if (k == "cpu") {
            c.cpus = parse_ints(v);
            c.has_cpu = !c.cpus.empty();
        } else if (k == "sched") {
            // "<policy>[:<prio>]"
            const size_t c2 = v.find(':');
            const std::string pol = (c2 == std::string::npos) ? v : v.substr(0, c2);
            c.policy = policy_of(pol);
            if (c2 != std::string::npos) {
                try { c.prio = std::stoi(v.substr(c2 + 1)); } catch (...) {}
            }
        } else if (k == "nice") {
            try { c.nice = std::stoi(v); c.has_nice = true; } catch (...) {}
        } else if (k == "dl") {
            const auto p = parse_ints(v);  // reuse: runtime,deadline,period
            if (p.size() == 3) {
                c.dl_runtime  = static_cast<uint64_t>(p[0]);
                c.dl_deadline = static_cast<uint64_t>(p[1]);
                c.dl_period   = static_cast<uint64_t>(p[2]);
            }
        }
    }
    return c;
}

// SCHED_DEADLINE has no glibc wrapper — call sched_setattr directly.
struct sched_attr_t {
    uint32_t size;
    uint32_t sched_policy;
    uint64_t sched_flags;
    int32_t  sched_nice;
    uint32_t sched_priority;
    uint64_t sched_runtime;
    uint64_t sched_deadline;
    uint64_t sched_period;
};

}  // namespace

void apply_node_affinity(pthread_t th, const char* node, const char* spec) {
    if (th == 0 || !node || !spec || !*spec) return;
    std::string fields;
    if (!find_record(spec, node, fields)) return;
    const NodeCfg c = parse_fields(fields);

    // ---- CPU affinity (needs no privilege) ----
    if (c.has_cpu) {
        cpu_set_t set;
        CPU_ZERO(&set);
        for (int cpu : c.cpus) {
            if (cpu >= 0 && cpu < CPU_SETSIZE) CPU_SET(cpu, &set);
        }
        int e = pthread_setaffinity_np(th, sizeof(set), &set);
        if (e != 0) {
            process_logger().warn(std::string("node ") + node +
                ": setaffinity failed: " + std::strerror(e));
        }
    }

    // ---- Scheduler ----
    if (c.policy < 0) return;

#ifdef SCHED_DEADLINE
    if (c.policy == SCHED_DEADLINE) {
        sched_attr_t attr{};
        attr.size           = sizeof(attr);
        attr.sched_policy   = SCHED_DEADLINE;
        attr.sched_runtime  = c.dl_runtime;
        attr.sched_deadline = c.dl_deadline;
        attr.sched_period   = c.dl_period;
        // sched_setattr applies to a TID; pthread_t isn't a tid, so DEADLINE
        // must be set from the node thread itself (tid 0 = caller). main.cc
        // can't do that here — log that DEADLINE is unsupported via this path.
        process_logger().warn(std::string("node ") + node +
            ": SCHED_DEADLINE must be set on the node's own thread "
            "(not yet wired) — skipping");
        (void)attr;
        return;
    }
#endif

    sched_param sp{};
    if (c.policy == SCHED_FIFO || c.policy == SCHED_RR) {
        sp.sched_priority = c.prio;
    } else {
        sp.sched_priority = 0;  // OTHER/BATCH/IDLE use 0; nice via setpriority
    }
    int e = pthread_setschedparam(th, c.policy, &sp);
    if (e != 0) {
        process_logger().warn(std::string("node ") + node +
            ": setschedparam(policy=" + std::to_string(c.policy) +
            ", prio=" + std::to_string(sp.sched_priority) + ") failed: " +
            std::strerror(e) + " (rtprio needs CAP_SYS_NICE)");
    }
    // nice for OTHER/BATCH would need the node's TID (setpriority is per-PID/TID,
    // not pthread_t) — left for the node-thread-side apply, same as DEADLINE.
    (void)c.has_nice;
}

}  // namespace runtime
}  // namespace theia
