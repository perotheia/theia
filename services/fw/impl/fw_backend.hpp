// fw_backend — the nftables generate + merge + apply plane behind FwDaemon.
//
// APP-OWNED. FW is a control plane, NOT a packet path: this backend BUILDS an
// `inet theia_fw` table text (default-drop input, allow-list the DMZ TCP ports +
// loopback + established/related), MERGES any config/fw.d/*.nft fragments as
// extra `input` rules, and APPLIES the whole thing atomically via `nft -f -`.
// The kernel enforces; the FC just authors the policy.
//
// SCOPE: the IP boundary only. The inter-FC transport is AF_TIPC, which nft's
// ip/inet tables can't filter — so this governs com's gRPC DMZ (:7700/:7710/
// :7711) + etcd (:2379), not the TIPC mesh.
//
// Graceful-degrade: no `nft` binary / no privilege → apply returns false + the
// diagnostic, and the FC reports FW_DEGRADED rather than crashing.

#pragma once

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <linux/capability.h>   // CAP_NET_ADMIN + __user_cap_*_struct
#include <string>
#include <sys/prctl.h>          // PR_CAP_AMBIENT_RAISE
#include <sys/syscall.h>        // SYS_capget / SYS_capset (raw, no libcap dep)
#include <unistd.h>             // syscall
#include <vector>

namespace ara::fw {

// FwState ordinals (.art): 0=UNKNOWN 1=APPLIED 2=DEGRADED 3=DISABLED.
enum FState : int { F_UNKNOWN = 0, F_APPLIED = 1, F_DEGRADED = 2, F_DISABLED = 3 };

struct ApplyResult {
    bool        ok = false;
    int         rule_count = 0;       // input-chain rules in the generated table
    int         override_count = 0;   // config/fw.d/*.nft fragments merged
    std::string message;
};

namespace fw_detail {

inline std::string run_(const std::string& cmd) {
    std::string out;
    FILE* p = ::popen((cmd + " 2>&1").c_str(), "r");
    if (!p) return out;
    char buf[256];
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    ::pclose(p);
    return out;
}

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

// Split "7700,7710,2379" → ["7700","7710","2379"], trimming blanks.
inline std::vector<std::string> split_ports_(const std::string& csv) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : csv) {
        if (c == ',' || c == ' ') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Collect *.nft fragments from a directory, sorted-ish (readdir order). Returns
// the concatenated fragment text + a count. Each fragment's lines are emitted
// inside the generated `input` chain, so a fragment is just bare nft rules
// (e.g. `tcp dport 8080 accept`).
inline std::string collect_overrides_(const std::string& dir, int& count) {
    count = 0;
    std::string merged;
    DIR* d = ::opendir(dir.c_str());
    if (!d) return merged;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        std::string name = e->d_name;
        if (name.size() < 5) continue;
        if (name.compare(name.size() - 4, 4, ".nft") != 0) continue;
        std::string body = slurp_(dir + "/" + name);
        if (body.empty()) continue;
        merged += "        # --- config/fw.d/" + name + " ---\n";
        // Indent each fragment line into the chain.
        std::string line;
        for (char c : body) {
            if (c == '\n') { merged += "        " + line + "\n"; line.clear(); }
            else line += c;
        }
        if (!line.empty()) merged += "        " + line + "\n";
        ++count;
    }
    ::closedir(d);
    return merged;
}

// Raise CAP_NET_ADMIN into this process's AMBIENT set so a spawned `nft` (a
// separate binary with no file cap of its own) inherits it Effective. A file
// capability is NOT inherited across execvp by default — same two-step the
// supervisor uses for CAP_SYS_NICE: (1) add the cap to our INHERITABLE set
// (PR_CAP_AMBIENT_RAISE needs it in both Permitted AND Inheritable; `setcap +eip`
// populates the FILE's inheritable, not the running process's, so set it
// explicitly via capset); (2) raise it into AMBIENT. Idempotent + best-effort: a
// silent no-op when fw lacks CAP_NET_ADMIN in Permitted (no setcap on the
// binary) — apply then degrades honestly. Call once before the first nft spawn.
inline void raise_net_admin_ambient() {
#ifndef CAP_TO_INDEX
#define CAP_TO_INDEX(x) ((x) >> 5)
#endif
#ifndef CAP_TO_MASK
#define CAP_TO_MASK(x) (1u << ((x) & 31))
#endif
    __user_cap_header_struct hdr{};
    hdr.version = _LINUX_CAPABILITY_VERSION_3;
    hdr.pid = 0;  // self
    __user_cap_data_struct data[2]{};
    if (::syscall(SYS_capget, &hdr, data) == 0) {
        const int idx = CAP_TO_INDEX(CAP_NET_ADMIN);
        const uint32_t bit = CAP_TO_MASK(CAP_NET_ADMIN);
        data[idx].inheritable |= bit;          // need Inh for the ambient raise
        ::syscall(SYS_capset, &hdr, data);     // best-effort
    }
#if defined(PR_CAP_AMBIENT) && defined(PR_CAP_AMBIENT_RAISE)
    ::prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_NET_ADMIN, 0, 0);
#endif
}

}  // namespace fw_detail

// Build the `inet theia_fw` ruleset text. `policy` is "drop" (deny-by-default)
// or "accept". DMZ ports become a single `tcp dport { … } accept`. Override
// fragments from fw_d_dir are appended inside the input chain. Returns the text
// + the override count + a rule count (the fixed rules + 1 dmz set + overrides).
inline std::string build_ruleset(const std::string& dmz_csv,
                                 const std::string& policy,
                                 const std::string& fw_d_dir,
                                 int& rule_count, int& override_count) {
    using namespace fw_detail;
    auto ports = split_ports_(dmz_csv);
    const std::string pol = (policy == "accept") ? "accept" : "drop";

    std::string s;
    s += "table inet theia_fw {\n";
    s += "    chain input {\n";
    s += "        type filter hook input priority 0; policy " + pol + ";\n";
    s += "        ct state established,related accept\n";
    s += "        ct state invalid drop\n";
    s += "        iif lo accept\n";
    int rules = 4;   // the four fixed rules above
    if (!ports.empty()) {
        s += "        tcp dport { ";
        for (size_t i = 0; i < ports.size(); ++i) {
            s += ports[i];
            if (i + 1 < ports.size()) s += ", ";
        }
        s += " } accept\n";
        ++rules;
    }
    // Merge hand-authored overrides (advisory line count rolled into rules).
    std::string ov = collect_overrides_(fw_d_dir, override_count);
    if (!ov.empty()) s += ov;
    rules += override_count;
    s += "    }\n";
    s += "}\n";
    rule_count = rules;
    return s;
}

// Apply a ruleset text atomically: flush our table then load the new one via
// `nft -f -`. Returns ok + a diagnostic. nft replaces the named table wholesale,
// so a stale theia_fw is cleaned. No `nft` / no privilege → ok=false.
inline ApplyResult apply_ruleset(const std::string& ruleset,
                                 int rule_count, int override_count) {
    using namespace fw_detail;
    ApplyResult r;
    r.rule_count = rule_count;
    r.override_count = override_count;

    if (run_("command -v nft").empty()) {
        r.ok = false;
        r.message = "nft not found — firewall plane unmanaged";
        return r;
    }
    // Push CAP_NET_ADMIN into the ambient set so the spawned `nft` inherits it
    // (the file cap on `fw` doesn't transfer across execvp on its own).
    fw_detail::raise_net_admin_ambient();
    // Delete our table first (ignore "No such file" on the first apply), then
    // load the fresh one. Pipe the ruleset straight to `nft -f -` (stdin) — no
    // temp file (avoids ownership/sticky-/tmp surprises across uid changes).
    // `nft -f` is atomic; an error leaves nft unchanged.
    run_("nft delete table inet theia_fw");
    std::string out;
    {
        FILE* p = ::popen("nft -f - 2>&1", "w");
        if (!p) { r.ok = false; r.message = "cannot spawn nft"; return r; }
        ::fwrite(ruleset.data(), 1, ruleset.size(), p);
        int rc = ::pclose(p);
        // popen("w") can't capture stdout; rely on the post-apply verify below
        // for the success signal, and the exit code for a hard failure.
        if (rc != 0) out = "nft -f returned non-zero";
    }
    if (!out.empty()) {
        // nft prints errors to stderr (merged into out); non-empty = failure.
        r.ok = false;
        auto nl = out.find('\n');   // trim to one line for the status message
        r.message = "nft apply failed: " + (nl == std::string::npos
                    ? out : out.substr(0, nl));
        return r;
    }
    // VERIFY the table is actually live — `nft -f` can return 0 without
    // installing the table on a host where the caller lacks CAP_NET_ADMIN (it
    // operates on a private netns / silently no-ops). A missing table after a
    // clean exit = no privilege → DEGRADED, so the status is honest.
    if (run_("nft list table inet theia_fw").find("chain input") ==
        std::string::npos) {
        r.ok = false;
        r.message = "nft applied 0 (no CAP_NET_ADMIN? table not live)";
        return r;
    }
    r.ok = true;
    r.message = "applied " + std::to_string(rule_count) + " rules (" +
                std::to_string(override_count) + " overrides)";
    return r;
}

// Flush the theia_fw table (the DISABLED path). Best-effort.
inline void flush_ruleset() {
    fw_detail::run_("nft delete table inet theia_fw");
}

}  // namespace ara::fw
