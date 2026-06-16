// proc_detector — the userspace (no-eBPF) IDS sensor behind IdsmDaemon.
//
// APP-OWNED. Implements the IDSM rule catalog Categories A/C/D/H from signals
// available WITHOUT eBPF (docs/autosar/services/idsm.md §3) — and WITHOUT
// shelling out: the listening-socket inventory comes from NETLINK_SOCK_DIAG
// (netlink_diag.hpp, no `ss`), the ELF hash from an in-process SHA-256
// (sha256.hpp, no `sha256sum`). It DIFFS those against the manifest-derived
// allow-lists in IdsmConfig. Edge-detected: a violation is emitted ONCE (when it
// appears), not every poll, so the firehose isn't flooded. (The only remaining
// popen is fw's nft-counter read for Cat B → moves to libnftnl.)
//
//   Cat A  IDSM_UNEXPECTED_SERVICE_ENDPOINT   — a process listens on an IP port
//          not in expected_listeners (a TIPC-only FC seen on TCP). sev 5.
//   Cat C  IDSM_UNEXPECTED_LISTENER           — same signal, framed as "a new
//          listener appeared"; emitted on the bind edge. sev 4. (A is the
//          manifest-violation flavour; C is the change-detection flavour — we
//          emit A when the comm is a known FC speaking an unexpected port, C
//          when the listener is wholly unaccounted.)
//   Cat D  IDSM_GRPC_UNAUTHORIZED_SERVER      — a process NOT in grpc_servers
//          listens on a gRPC DMZ port (7700/7710/7711). sev 5.
//   Cat H  IDSM_APPLICATION_INTEGRITY_FAILURE — a running ELF's SHA256 != the
//          manifest digest for its comm. sev 5.
//
// The catalog's "manager, not sensor" property holds: this reads kernel-exposed
// inventory; it never inspects packets or sits in a data path. Graceful: missing
// `ss` / unreadable /proc → that rule yields nothing (never throws).

#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nftables/libnftables.h>   // in-process nft counter read (no exec)

#include "ids_backend.hpp"     // DetectionEv
#include "netlink_diag.hpp"    // netlink_listeners — sock_diag, no `ss` exec
#include "sha256.hpp"          // in-process SHA-256, no `sha256sum` exec

namespace ara::idsm {

namespace proc_detail {

// (run_ removed — idsm has NO popen now: ss→netlink, sha256sum→in-process,
// nft→libnftables.)

// Split "a:1,b:2" → [("a","1"),("b","2")]. Tolerates spaces.
inline std::vector<std::pair<std::string, std::string>>
split_pairs_(const std::string& csv) {
    std::vector<std::pair<std::string, std::string>> out;
    std::string item;
    auto flush = [&] {
        if (item.empty()) return;
        auto c = item.find(':');
        if (c != std::string::npos)
            out.emplace_back(item.substr(0, c), item.substr(c + 1));
        item.clear();
    };
    for (char ch : csv) { if (ch == ',') flush(); else if (ch != ' ') item += ch; }
    flush();
    return out;
}

inline std::set<std::string> split_set_(const std::string& csv) {
    std::set<std::string> out;
    std::string item;
    for (char ch : csv) {
        if (ch == ',') { if (!item.empty()) out.insert(item); item.clear(); }
        else if (ch != ' ') item += ch;
    }
    if (!item.empty()) out.insert(item);
    return out;
}

// The listening-socket inventory is now NETLINK_SOCK_DIAG (netlink_diag.hpp),
// already scoped to the FCs — no `ss` exec, no text parsing. scan() uses the
// NlListener it returns directly.

// SHA256 of a file — in-process (sha256.hpp), no `sha256sum` exec. A cheap
// integrity spot-check; the crypto FC owns real signature verification.
inline std::string sha256_file(const std::string& path) {
    return ara::idsm::sha256_file_inproc(path);
}

// Parse `nft -j list counters` JSON → [(fc, packets)] for each idsm_b_<fc>
// counter. PURE (testable): walk for {"counter": {"name":"idsm_b_<fc>", …
// "packets": N}}. Tolerates nft's spacing ("name": vs "name":).
inline std::vector<std::pair<std::string, uint64_t>>
parse_nft_counters_(const std::string& j) {
    std::vector<std::pair<std::string, uint64_t>> out;
    size_t pos = 0;
    const std::string marker = "idsm_b_";
    // Find each "name"... "idsm_b_<fc>"; nft emits "name": "idsm_b_per".
    while ((pos = j.find("\"name\"", pos)) != std::string::npos) {
        size_t q1 = j.find('"', j.find(':', pos) + 0);
        // value string starts at the first '"' after the ':'
        size_t colon = j.find(':', pos);
        size_t vs = j.find('"', colon);
        if (vs == std::string::npos) break;
        size_t ve = j.find('"', vs + 1);
        if (ve == std::string::npos) break;
        std::string name = j.substr(vs + 1, ve - vs - 1);
        pos = ve + 1;
        if (name.rfind(marker, 0) != 0) continue;   // not an idsm_b_ counter
        std::string fc = name.substr(marker.size());
        size_t pk = j.find("\"packets\"", ve);
        uint64_t pkts = 0;
        if (pk != std::string::npos) {
            size_t pc = j.find(':', pk);
            if (pc != std::string::npos)
                pkts = std::strtoull(j.c_str() + pc + 1, nullptr, 10);
        }
        out.emplace_back(fc, pkts);
        (void)q1;
    }
    return out;
}

// Read fw's per-FC egress-drop counters (Cat B) IN-PROCESS via libnftables (no
// `nft` exec): `list counters table inet theia_fw` in JSON. fw declares
// idsm_b_<fc> before its `socket cgroupv2 … drop` rules, so a nonzero packet
// count means that FC tried a denied egress. Empty if the table is absent.
inline std::string nft_counters_json_() {
    struct nft_ctx* ctx = nft_ctx_new(NFT_CTX_DEFAULT);
    if (!ctx) return "";
    nft_ctx_output_set_flags(ctx, NFT_CTX_OUTPUT_JSON);
    nft_ctx_buffer_output(ctx);
    nft_ctx_buffer_error(ctx);
    std::string out;
    if (nft_run_cmd_from_buffer(ctx, "list counters table inet theia_fw") == 0) {
        const char* o = nft_ctx_get_output_buffer(ctx);
        if (o) out = o;
    }
    nft_ctx_free(ctx);
    return out;
}

inline std::vector<std::pair<std::string, uint64_t>> nft_egress_drops() {
    return parse_nft_counters_(nft_counters_json_());
}

}  // namespace proc_detail

// Stateful userspace detector. The FC owns one instance; call scan() each tick.
// It remembers what it has already reported (edge detection) so each distinct
// violation fires once.
class ProcDetector {
public:
    void configure(const std::string& expected_listeners,
                   const std::string& grpc_servers,
                   const std::string& elf_digests) {
        expected_.clear();
        for (auto& [comm, port] : proc_detail::split_pairs_(expected_listeners))
            expected_.insert(comm + ":" + port);
        grpc_servers_ = proc_detail::split_set_(grpc_servers);
        digests_.clear();
        for (auto& [comm, hex] : proc_detail::split_pairs_(elf_digests))
            digests_[comm] = hex;
        // A config change re-arms edge detection (re-report under the new policy).
        reported_.clear();
        elf_checked_.clear();
    }

    // The gRPC DMZ ports (Cat D scope). Kept in sync with com's servers + fw.
    static bool is_grpc_port(int port) {
        return port == 7700 || port == 7710 || port == 7711;
    }

    // Scan once; return the NEW violations since the last scan (edge-detected).
    // `supervisor_pid` SCOPES the scan to Theia FCs (children of the supervisor)
    // — IDSM watches the manifest-known process set, not arbitrary host
    // processes. A listener whose pid isn't a supervisor child is ignored (it's
    // some unrelated host service, not part of the deployment).
    std::vector<DetectionEv> scan(int supervisor_pid) {
        using namespace proc_detail;
        std::vector<DetectionEv> out;
        // Listening sockets via NETLINK_SOCK_DIAG (no `ss` exec), already SCOPED
        // to the FCs (children of supervisor_pid) by the inode→pid join.
        auto listeners = netlink_listeners(supervisor_pid);
        std::set<std::string> live;   // keys seen this scan (to expire reported_)

        for (const auto& l : listeners) {
            if (l.pid <= 0) continue;
            const std::string key = l.comm + ":" + std::to_string(l.port);
            live.insert(key);

            // Cat D — a non-sanctioned process on a gRPC DMZ port.
            if (is_grpc_port(l.port) && !grpc_servers_.count(l.comm)) {
                emit_once_(out, "catD:" + key, 5, "IDSM_GRPC_UNAUTHORIZED_SERVER",
                           l.comm + "/" + std::to_string(l.pid),
                           "grpc:" + std::to_string(l.port));
            }

            // Cat A / C — a listener not in the expected set.
            if (!expected_.count(key)) {
                // A known FC comm on an unexpected port = manifest drift (A);
                // an entirely unaccounted comm = a new/rogue listener (C).
                const bool known_fc = comm_is_known_fc_(l.comm);
                if (known_fc) {
                    emit_once_(out, "catA:" + key, 5,
                               "IDSM_UNEXPECTED_SERVICE_ENDPOINT",
                               l.comm + "/" + std::to_string(l.pid),
                               "tcp:" + std::to_string(l.port));
                } else {
                    emit_once_(out, "catC:" + key, 4, "IDSM_UNEXPECTED_LISTENER",
                               l.comm + "/" + std::to_string(l.pid),
                               "tcp:" + std::to_string(l.port));
                }
            }

            // Cat H — integrity of the listening process's ELF (once per pid).
            check_elf_(out, l.comm, l.pid);
        }

        // Expire edges that are gone, so a re-appearance re-fires.
        for (auto it = reported_.begin(); it != reported_.end();) {
            // keep Cat-H keys (pid-scoped, expire on pid change) + live socket keys
            if (it->rfind("elfH:", 0) == 0 || it->rfind("catB:", 0) == 0 ||
                live.count(it->substr(it->find(':') + 1)))
                ++it;
            else it = reported_.erase(it);
        }

        // Cat B — denied egress, correlated from fw's nft drop counters. A
        // nonzero packet delta on idsm_b_<fc> means that FC tried a destination
        // fw's per-cgroup egress allow-list denied. This is the DETECT side of
        // fw's ENFORCE — no eBPF; the enforcement + the signal both ride nft.
        for (const auto& [fc, pkts] : proc_detail::nft_egress_drops()) {
            uint64_t prev = last_drops_.count(fc) ? last_drops_[fc] : 0;
            if (pkts > prev) {
                last_drops_[fc] = pkts;
                // Re-key per cumulative count so each new burst re-fires.
                emit_once_(out, "catB:" + fc + ":" + std::to_string(pkts), 4,
                           "IDSM_UNEXPECTED_OUTBOUND_CONNECTION", fc,
                           "denied-egress(" + std::to_string(pkts - prev) +
                           " pkts)");
            } else {
                last_drops_[fc] = pkts;
            }
        }
        return out;
    }

    // The known-FC comm set lets scan() tell Cat A (a real FC misbehaving) from
    // Cat C (a wholly unknown listener). Seeded from expected_listeners' comms +
    // grpc_servers; the FC can also pass the manifest process list.
    void set_known_fcs(const std::set<std::string>& fcs) { known_fcs_ = fcs; }

private:
    std::set<std::string> expected_;       // "comm:port" the deployment expects
    std::set<std::string> grpc_servers_;   // comms allowed to serve gRPC
    std::map<std::string, std::string> digests_;   // comm → expected sha256
    std::set<std::string> known_fcs_;      // comms that are real platform FCs
    std::set<std::string> reported_;       // edge-dedup keys
    std::map<int, std::string> elf_checked_;   // pid → sha already verified
    std::map<std::string, uint64_t> last_drops_;   // fc → last egress-drop count

    bool comm_is_known_fc_(const std::string& comm) const {
        if (known_fcs_.count(comm)) return true;
        // Fall back to the expected_/grpc comms (those are FCs by definition).
        for (const auto& e : expected_)
            if (e.rfind(comm + ":", 0) == 0) return true;
        return grpc_servers_.count(comm) > 0;
    }

    void emit_once_(std::vector<DetectionEv>& out, const std::string& key,
                    uint32_t sev, const char* sig, const std::string& src,
                    const std::string& dst) {
        if (!reported_.insert(key).second) return;   // already reported
        DetectionEv ev;
        ev.severity = sev; ev.signature = sig; ev.src = src; ev.dst = dst;
        out.push_back(std::move(ev));
    }

    void check_elf_(std::vector<DetectionEv>& out, const std::string& comm,
                    int pid) {
        if (pid <= 0) return;
        auto dit = digests_.find(comm);
        if (dit == digests_.end()) return;            // no expected digest → skip
        auto cit = elf_checked_.find(pid);
        std::string exe = "/proc/" + std::to_string(pid) + "/exe";
        std::string sha = proc_detail::sha256_file(exe);
        if (sha.empty()) return;
        if (cit != elf_checked_.end() && cit->second == sha) return;  // checked OK
        elf_checked_[pid] = sha;
        if (sha != dit->second) {
            emit_once_(out, "elfH:" + comm + ":" + sha, 5,
                       "IDSM_APPLICATION_INTEGRITY_FAILURE",
                       comm + "/" + std::to_string(pid), "sha256:" + sha);
        }
    }
};

}  // namespace ara::idsm
