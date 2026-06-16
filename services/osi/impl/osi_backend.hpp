// osi_backend — the cgroup v2 + /proc resource plane behind OsiCtl.
//
// APP-OWNED. A THIN control plane over the kernel:
//   - discover_fcs(): the FC processes = children of the supervisor (PPID
//     match), comm read from /proc/<pid>/comm.
//   - sample_proc(): /proc/<pid>/stat utime+stime jiffies (Δ over the poll
//     interval ÷ wall Δ ÷ clock-tick → cpu%) + /proc/<pid>/status VmRSS.
//   - read/write cgroup v2: <root>/<slice>/<fc>/{cpu.max,memory.high}. The
//     supervisor places children into the slice; OSI owns the knobs. Reads
//     degrade to "unlimited" where the cgroup file is absent/unreadable;
//     writes report failure rather than throwing.
//
// (The NVIDIA Orin power mode moved to services/shwa.) The FC NEVER forks a
// child (the supervisor / EM owns lifecycle) and NEVER sits in any data path.

#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace ara::osi {

struct ProcSample {
    std::string fc;            // FC short name (= comm = cgroup sub-dir)
    int         pid = -1;
    uint64_t    cpu_jiffies = 0;   // utime+stime from /proc/<pid>/stat
    uint64_t    rss_bytes = 0;     // VmRSS from /proc/<pid>/status
};

namespace osi_detail {

inline std::string slurp_(const std::string& path) {
    std::string out;
    FILE* f = ::fopen(path.c_str(), "r");
    if (!f) return out;
    char buf[512];
    size_t n;
    while ((n = ::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    ::fclose(f);
    return out;
}

inline bool write_(const std::string& path, const std::string& val) {
    FILE* f = ::fopen(path.c_str(), "w");
    if (!f) return false;
    size_t n = ::fwrite(val.data(), 1, val.size(), f);
    bool ok = (n == val.size());
    ok = (::fclose(f) == 0) && ok;
    return ok;
}

// /proc/<pid>/comm, trimmed of the trailing newline.
inline std::string comm_(int pid) {
    std::string c = slurp_("/proc/" + std::to_string(pid) + "/comm");
    while (!c.empty() && (c.back() == '\n' || c.back() == '\r')) c.pop_back();
    return c;
}

// The PPID from /proc/<pid>/stat (field 4, after the "(comm)" which may contain
// spaces/parens — so scan past the last ')').
inline int ppid_(int pid) {
    std::string s = slurp_("/proc/" + std::to_string(pid) + "/stat");
    auto rp = s.rfind(')');
    if (rp == std::string::npos) return -1;
    // After ')' : " <state> <ppid> ...". Skip the space + state token.
    int st_dummy, ppid = -1;
    if (std::sscanf(s.c_str() + rp + 1, " %c %d", (char*)&st_dummy, &ppid) >= 2)
        return ppid;
    return -1;
}

}  // namespace osi_detail

// ---- A. resource accounting -----------------------------------------------

// Discover the FC processes: direct children of `supervisor_pid`. Returns a
// sample (pid + comm + cpu jiffies + rss) per child, so the caller can Δ cpu
// against the previous poll. `known` filters to FC short names we manage (empty
// = accept every supervisor child).
inline std::vector<ProcSample> discover_fcs(
        int supervisor_pid, const std::vector<std::string>& known) {
    using namespace osi_detail;
    std::vector<ProcSample> out;
    DIR* d = ::opendir("/proc");
    if (!d) return out;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        // numeric entries only = pids.
        char* end = nullptr;
        long pid = std::strtol(e->d_name, &end, 10);
        if (end == e->d_name || *end != '\0') continue;
        if (ppid_(static_cast<int>(pid)) != supervisor_pid) continue;

        ProcSample ps;
        ps.pid = static_cast<int>(pid);
        ps.fc  = comm_(ps.pid);
        if (!known.empty()) {
            bool match = false;
            for (const auto& k : known) if (k == ps.fc) { match = true; break; }
            if (!match) continue;
        }
        // CPU jiffies = utime(14)+stime(15) of /proc/<pid>/stat (after comm).
        std::string s = slurp_("/proc/" + std::to_string(pid) + "/stat");
        auto rp = s.rfind(')');
        if (rp != std::string::npos) {
            unsigned long ut = 0, stime = 0;
            // fields after ')': state ppid pgrp session tty tpgid flags minflt
            // cminflt majflt cmajflt utime stime ... → utime is the 12th token.
            unsigned long toks[15] = {0};
            int got = std::sscanf(s.c_str() + rp + 1,
                " %*c %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
                &toks[0], &toks[1], &toks[2], &toks[3], &toks[4], &toks[5],
                &toks[6], &toks[7], &toks[8], &toks[9], &ut, &stime);
            (void)got;
            ps.cpu_jiffies = ut + stime;
        }
        // RSS from /proc/<pid>/status VmRSS (kB).
        std::string status = slurp_("/proc/" + std::to_string(pid) + "/status");
        auto vp = status.find("VmRSS:");
        if (vp != std::string::npos) {
            unsigned long kb = 0;
            std::sscanf(status.c_str() + vp, "VmRSS: %lu kB", &kb);
            ps.rss_bytes = static_cast<uint64_t>(kb) * 1024ull;
        }
        out.push_back(std::move(ps));
    }
    ::closedir(d);
    return out;
}

// cpu% from a jiffies delta over a wall-clock interval (ms). Returns % of ONE
// core (so a fully-busy 2-thread process can read ~200).
inline double cpu_pct_from_delta(uint64_t djiffies, uint64_t dwall_ms) {
    if (dwall_ms == 0) return 0.0;
    long hz = ::sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;
    double dsec = static_cast<double>(dwall_ms) / 1000.0;
    return (static_cast<double>(djiffies) / static_cast<double>(hz)) / dsec * 100.0;
}

// The cgroup v2 dir for one FC under the Theia slice.
inline std::string fc_cgroup_dir(const std::string& root,
                                 const std::string& slice,
                                 const std::string& fc) {
    return root + "/" + slice + "/" + fc;
}

// Ensure the Theia slice exists with the cpu+memory controllers delegated to its
// children. Idempotent. Needs write access to the cgroup tree (OSI runs as a
// root child of the root supervisor in the real stack); a no-op + false on a
// non-delegated / unprivileged host (the dev host run-as-user) — accounting
// still works, only limit-writing is unavailable. The controllers are enabled in
// the SLICE's subtree_control so each per-FC sub-cgroup can carry cpu.max etc.
inline bool ensure_slice(const std::string& root, const std::string& slice) {
    using namespace osi_detail;
    std::string dir = root + "/" + slice;
    // mkdir is fine if it already exists (EEXIST). 0 or EEXIST → proceed.
    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return false;
    // Delegate cpu+memory to the slice's children (so <fc>/cpu.max is writable).
    // Best-effort: if the root already has them in subtree_control this succeeds.
    write_(dir + "/cgroup.subtree_control", "+cpu +memory");
    return true;
}

// Create the per-FC sub-cgroup and move `pid` into it. Idempotent (writing a pid
// already in the cgroup is a no-op). Returns false (no throw) where the tree
// isn't writable. `cgroup.procs` moves the whole process (all its node threads).
inline bool place_pid(const std::string& cg_dir, int pid) {
    using namespace osi_detail;
    if (::mkdir(cg_dir.c_str(), 0755) != 0 && errno != EEXIST) return false;
    return write_(cg_dir + "/cgroup.procs", std::to_string(pid));
}

// Read the applied cpu.max cap (% of one core; 0 = "max"/unlimited).
inline uint32_t read_cpu_max_pct(const std::string& cg_dir) {
    using namespace osi_detail;
    std::string v = slurp_(cg_dir + "/cpu.max");   // "<quota> <period>" or "max <period>"
    if (v.empty() || v.compare(0, 3, "max") == 0) return 0;
    unsigned long quota = 0, period = 0;
    if (std::sscanf(v.c_str(), "%lu %lu", &quota, &period) == 2 && period)
        return static_cast<uint32_t>(quota * 100ull / period);
    return 0;
}

// Read the applied memory.high (bytes; 0 = "max"/unlimited).
inline uint64_t read_mem_high(const std::string& cg_dir) {
    using namespace osi_detail;
    std::string v = slurp_(cg_dir + "/memory.high");
    if (v.empty() || v.compare(0, 3, "max") == 0) return 0;
    return std::strtoull(v.c_str(), nullptr, 10);
}

// Apply a per-FC cgroup v2 limit. cpu_max_pct=0 → "max" (unlimited); else
// quota=pct*period/100 with a 100ms period. mem_high=0 → "max". Returns false +
// fills `err` if the cgroup dir isn't writable (degraded host / no delegation).
inline bool apply_limit(const std::string& cg_dir, uint32_t cpu_max_pct,
                        uint64_t mem_high, std::string& err) {
    using namespace osi_detail;
    // cpu.max
    std::string cpu_val;
    if (cpu_max_pct == 0) cpu_val = "max 100000";
    else cpu_val = std::to_string(static_cast<unsigned long>(cpu_max_pct) * 1000ul)
                   + " 100000";   // pct% of one core over a 100000us period
    if (!write_(cg_dir + "/cpu.max", cpu_val)) {
        err = "cpu.max write failed (" + cg_dir + " — cgroup v2 delegated?)";
        return false;
    }
    // memory.high
    std::string mem_val = (mem_high == 0) ? "max" : std::to_string(mem_high);
    if (!write_(cg_dir + "/memory.high", mem_val)) {
        err = "memory.high write failed (" + cg_dir + ")";
        return false;
    }
    return true;
}

// (The Orin power-mode functions — on_jetson / read_power_mode /
// apply_power_mode — moved to services/shwa with the rest of the Jetson plane.)

}  // namespace ara::osi
