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

#include "impl/nft_lib.hpp"     // in-process nftables (libnftables, no exec)
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

// (run_ removed — nft is now in-process via nft_lib::run; the only remaining
// file access is slurp_ below, a plain fopen of config/fw.d fragments.)

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

// Parse the per-FC egress policy "fc=cidr,cidr;fc2=cidr" → [(fc, [cidrs])].
// A cgroup that doesn't exist under the slice is skipped at emit time (nft would
// reject the rule), so a policy can name FCs that aren't placed yet.
inline std::vector<std::pair<std::string, std::vector<std::string>>>
parse_egress_(const std::string& policy) {
    std::vector<std::pair<std::string, std::vector<std::string>>> out;
    std::string cur;
    auto flush = [&] {
        if (cur.empty()) return;
        auto eq = cur.find('=');
        if (eq != std::string::npos) {
            std::string fc = cur.substr(0, eq);
            std::vector<std::string> cidrs;
            std::string c;
            for (char ch : cur.substr(eq + 1)) {
                if (ch == ',') { if (!c.empty()) cidrs.push_back(c); c.clear(); }
                else if (ch != ' ') c += ch;
            }
            if (!c.empty()) cidrs.push_back(c);
            if (!fc.empty()) out.emplace_back(fc, cidrs);
        }
        cur.clear();
    };
    for (char ch : policy) { if (ch == ';') flush(); else if (ch != ' ') cur += ch; }
    flush();
    return out;
}

// Does the FC's cgroup dir exist under the slice? (nft `socket cgroupv2` needs
// the path to resolve at rule-add time, else the whole `nft -f` fails.)
inline bool fc_cgroup_exists(const std::string& cgroup_root,
                             const std::string& slice, const std::string& fc) {
    std::string p = cgroup_root + "/" + slice + "/" + fc;
    FILE* f = ::fopen((p + "/cgroup.procs").c_str(), "r");
    if (f) { ::fclose(f); return true; }
    return false;
}

// Split a comma/space list into trimmed tokens (CIDRs, iface names — keeps
// non-digit chars, unlike split_ports_). "10.20.0.0/16, 10.30.0.0/16" → 2 toks.
inline std::vector<std::string> split_csv_(const std::string& csv) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : csv) {
        if (c == ',' || c == ' ' || c == '\t') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Custom site policy layered ONTO the comm-matrix baseline (FW.d). All optional;
// an empty/false field leaves the baseline behaviour unchanged.
struct FwPolicy {
    std::string grpc_client_cidrs;   // saddr-restrict the DMZ ports ("" = global)
    std::string vpn_iface;           // trusted inbound iface, e.g. "wg0" ("" = none)
    std::string forward_policy;      // "drop" → default-drop forward chain ("" = none)
    std::string output_policy;       // "drop" → default-drop output chain ("" = open)
    bool        log_drops = false;   // log+count dropped inbound (idsm/phm)
    uint32_t    log_drops_rate = 5;  // per-second cap on the FW_DROP log line
};

// Build the `inet theia_fw` ruleset text. `policy` is "drop" (deny-by-default)
// or "accept" for the INPUT chain. DMZ ports become a single
// `tcp dport { … } accept`. Override fragments from fw_d_dir are appended inside
// the input chain. When `egress_policy` is set, an OUTPUT chain enforces per-FC
// egress over osi's cgroup slices (socket cgroupv2 — nft-native, no eBPF): each
// FC's allowed CIDRs accept, the rest log "IDSM_B " (so idsm flags Cat B) + drop.
// `cgroup_root` (default /sys/fs/cgroup) + `egress_slice` locate the cgroups.
inline std::string build_ruleset(const std::string& dmz_csv,
                                 const std::string& policy,
                                 const std::string& fw_d_dir,
                                 const std::string& egress_policy,
                                 const std::string& egress_slice,
                                 const std::string& cgroup_root,
                                 const FwPolicy& custom,
                                 int& rule_count, int& override_count,
                                 int& egress_fc_count) {
    using namespace fw_detail;
    auto ports = split_ports_(dmz_csv);
    const std::string pol = (policy == "accept") ? "accept" : "drop";

    // Helper: emit "{ a, b, c }" from a token list (CIDRs / ports).
    auto nft_set = [](const std::vector<std::string>& v) {
        std::string out = "{ ";
        for (size_t i = 0; i < v.size(); ++i) {
            out += v[i];
            if (i + 1 < v.size()) out += ", ";
        }
        out += " }";
        return out;
    };

    int rules = 4;   // the four fixed input rules below
    // Table-level named counters MUST be declared before any chain that uses
    // them — accumulate them here and emit first.
    std::string counters;

    // ---- INPUT chain ----------------------------------------------------
    std::string in_chain;
    in_chain += "    chain input {\n";
    in_chain += "        type filter hook input priority 0; policy " + pol + ";\n";
    in_chain += "        ct state established,related accept\n";
    in_chain += "        ct state invalid drop\n";
    in_chain += "        iif lo accept\n";

    // FW.d: management VPN — a fully-trusted inbound interface. Emitted BEFORE
    // the dport rule so ssh/puppet/monitoring over the VPN bypass the saddr
    // restriction below.
    if (!custom.vpn_iface.empty()) {
        in_chain += "        iif \"" + custom.vpn_iface + "\" accept\n";
        ++rules;
    }
    if (!ports.empty()) {
        // FW.d: source-network restriction. When grpc_client_cidrs is set, only
        // those networks reach the DMZ ports (else they're globally exposed).
        auto cidrs = split_csv_(custom.grpc_client_cidrs);
        if (!cidrs.empty())
            in_chain += "        ip saddr " + nft_set(cidrs) +
                        " tcp dport " + nft_set(ports) + " accept\n";
        else
            in_chain += "        tcp dport " + nft_set(ports) + " accept\n";
        ++rules;
    }
    // Merge hand-authored overrides (advisory line count rolled into rules).
    std::string ov = collect_overrides_(fw_d_dir, override_count);
    if (!ov.empty()) in_chain += ov;
    rules += override_count;
    // FW.d: log + count dropped inbound (idsm/phm probe detection) as the LAST
    // input rule — rate-limited so a chatty DMZ can't flood the log. Only
    // meaningful under a default-drop policy (accept never reaches the tail).
    if (custom.log_drops && pol == "drop") {
        counters += "    counter fw_drop { }\n";
        in_chain += "        limit rate " + std::to_string(custom.log_drops_rate ?
                custom.log_drops_rate : 5) +
             "/second counter name \"fw_drop\" log prefix \"FW_DROP: \" drop\n";
        ++rules;
    }
    in_chain += "    }\n";

    // ---- FORWARD chain (FW.d): default-drop for a routing/DMZ host ------
    std::string fwd_chain;
    if (custom.forward_policy == "drop") {
        fwd_chain += "    chain forward {\n";
        fwd_chain += "        type filter hook forward priority 0; policy drop;\n";
        fwd_chain += "        ct state established,related accept\n";
        fwd_chain += "    }\n";
        rules += 2;
    }

    // ---- OUTPUT chain: per-FC EGRESS over osi's cgroup slices + optional
    //      default-drop egress (FW.d output_policy) ------------------------
    egress_fc_count = 0;
    std::string out_chain_rules;
    auto egress = parse_egress_(egress_policy);
    for (const auto& [fc, cidrs] : egress) {
        if (!fc_cgroup_exists(cgroup_root, egress_slice, fc))
            continue;   // FC not placed under the slice yet — skip (graceful)
        const std::string match =
            "socket cgroupv2 level 2 \"" + egress_slice + "/" + fc + "\"";
        if (!cidrs.empty())
            out_chain_rules += "        " + match + " ip daddr " +
                               nft_set(cidrs) + " accept\n";
        // loopback always allowed (TIPC/local), then default-deny this FC:
        // a NAMED COUNTER (per FC) so idsm can poll `nft list counter` for a
        // nonzero delta → Cat B IDSM_UNEXPECTED_OUTBOUND_CONNECTION (the
        // detect side of this enforce); plus a log prefix for forensics.
        out_chain_rules += "        " + match + " ip daddr 127.0.0.0/8 accept\n";
        out_chain_rules += "        " + match +
            " counter name \"idsm_b_" + fc + "\" log prefix \"IDSM_B " +
            fc + " \" drop\n";
        counters += "    counter idsm_b_" + fc + " { }\n";
        ++egress_fc_count;
    }
    // FW.d output_policy="drop": a default-drop output chain (established/related
    // + lo) is the "explicit egress" production pattern — the host originates
    // nothing not allow-listed. Without it (and no per-FC egress), no output
    // chain is emitted (egress stays open, today's behaviour).
    std::string out_chain;
    const bool drop_egress = (custom.output_policy == "drop");
    if (egress_fc_count > 0 || drop_egress) {
        out_chain += "    chain output {\n";
        out_chain += "        type filter hook output priority 0; policy " +
                     std::string(drop_egress ? "drop" : "accept") + ";\n";
        out_chain += "        ct state established,related accept\n";
        if (drop_egress) out_chain += "        oif lo accept\n";
        out_chain += out_chain_rules;
        out_chain += "    }\n";
        rules += egress_fc_count + (drop_egress ? 2 : 0);
    }

    // ---- assemble: counters first, then the chains ----------------------
    std::string s = "table inet theia_fw {\n";
    s += counters;
    s += in_chain;
    s += fwd_chain;
    s += out_chain;
    s += "}\n";
    rule_count = rules;
    return s;
}

// Apply a ruleset text atomically via libnftables (no `nft` exec). The ruleset
// (a `table inet theia_fw { … }` block) is prefixed with a `delete table` so the
// whole buffer replaces a stale table in one atomic nft transaction. Returns
// ok + a diagnostic; no privilege → ok=false (DEGRADED), verified by a read-back.
inline ApplyResult apply_ruleset(const std::string& ruleset,
                                 int rule_count, int override_count) {
    ApplyResult r;
    r.rule_count = rule_count;
    r.override_count = override_count;

    // CAP_NET_ADMIN must be effective for the netlink transaction. The file cap
    // on `fw` covers this process directly (no child exec now), but we still
    // raise it into the ambient/effective set defensively.
    fw_detail::raise_net_admin_ambient();

    // One atomic buffer: drop the old table (ignored if absent via `add`-style
    // tolerance — libnftables errors on delete-of-missing, so we run it as a
    // separate best-effort command first), then load the fresh ruleset.
    nft_lib::run("delete table inet theia_fw");   // best-effort (ignore error)
    std::string err = nft_lib::run(ruleset);
    if (!err.empty()) {
        r.ok = false;
        auto nl = err.find('\n');
        r.message = "nft apply failed: " + (nl == std::string::npos
                    ? err : err.substr(0, nl));
        return r;
    }
    // VERIFY the table is live — a clean return without CAP_NET_ADMIN can still
    // no-op on a private netns, so the read-back is the honest success signal.
    std::string listing;
    nft_lib::run("list table inet theia_fw", &listing);
    if (listing.find("chain input") == std::string::npos) {
        r.ok = false;
        r.message = "nft applied 0 (no CAP_NET_ADMIN? table not live)";
        return r;
    }
    r.ok = true;
    r.message = "applied " + std::to_string(rule_count) + " rules (" +
                std::to_string(override_count) + " overrides)";
    return r;
}

// Flush the theia_fw table (the DISABLED path). Best-effort, in-process.
inline void flush_ruleset() {
    nft_lib::run("delete table inet theia_fw");
}

}  // namespace ara::fw
