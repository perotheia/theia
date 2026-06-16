// proc_detector — the userspace (no-eBPF) IDS sensor behind IdsmDaemon.
//
// APP-OWNED. Implements the IDSM rule catalog Categories A/C/D/H from signals
// available WITHOUT eBPF (docs/autosar/services/idsm.md §3): it polls the
// listening-socket inventory (`ss -Htlnp` → comm/pid/port) + the running ELFs
// (/proc/<pid>/exe) and DIFFS against the manifest-derived allow-lists in
// IdsmConfig. Edge-detected: a violation is emitted ONCE (when it appears), not
// every poll, so the firehose isn't flooded.
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

#include "ids_backend.hpp"   // DetectionEv

namespace ara::idsm {

namespace proc_detail {

inline std::string run_(const std::string& cmd) {
    std::string out;
    FILE* p = ::popen((cmd + " 2>/dev/null").c_str(), "r");
    if (!p) return out;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    ::pclose(p);
    return out;
}

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

// One observed listening socket.
struct Listener {
    std::string comm;
    int         pid = -1;
    int         port = 0;
};

// Parse `ss -Htlnp` lines → listeners. A line looks like:
//   LISTEN 0 4096 *:7700 *:* users:(("com",pid=123,fd=42))
inline std::vector<Listener> scan_listeners() {
    std::vector<Listener> out;
    std::string s = run_("ss -Htlnp");
    size_t pos = 0;
    while (pos < s.size()) {
        size_t eol = s.find('\n', pos);
        std::string line = s.substr(pos, eol == std::string::npos
                                         ? std::string::npos : eol - pos);
        pos = (eol == std::string::npos) ? s.size() : eol + 1;
        if (line.empty()) continue;

        // The local address is the 4th whitespace field; the port is after the
        // last ':'. users:((...)) carries comm + pid.
        // Extract port.
        Listener l;
        // Find "users:((" to get comm/pid.
        auto u = line.find("users:((\"");
        if (u != std::string::npos) {
            size_t cstart = u + std::strlen("users:((\"");
            size_t cend = line.find('"', cstart);
            if (cend != std::string::npos) l.comm = line.substr(cstart, cend - cstart);
            auto pp = line.find("pid=", cend == std::string::npos ? u : cend);
            if (pp != std::string::npos) l.pid = std::atoi(line.c_str() + pp + 4);
        }
        // Port = digits after the last ':' in the local-addr field. Scan the
        // first 4 fields; the 4th is the local address.
        {
            int field = 0; size_t i = 0; std::string addr;
            while (i < line.size() && field < 4) {
                while (i < line.size() && line[i] == ' ') ++i;
                size_t start = i;
                while (i < line.size() && line[i] != ' ') ++i;
                ++field;
                if (field == 4) addr = line.substr(start, i - start);
            }
            auto colon = addr.rfind(':');
            if (colon != std::string::npos)
                l.port = std::atoi(addr.c_str() + colon + 1);
        }
        if (l.port > 0) out.push_back(std::move(l));
    }
    return out;
}

// The PPID from /proc/<pid>/stat (field after the "(comm)"). -1 on failure.
inline int ppid_(int pid) {
    std::string s;
    FILE* f = ::fopen(("/proc/" + std::to_string(pid) + "/stat").c_str(), "r");
    if (!f) return -1;
    char buf[512]; size_t n = ::fread(buf, 1, sizeof(buf) - 1, f); ::fclose(f);
    buf[n] = '\0'; s.assign(buf, n);
    auto rp = s.rfind(')');
    if (rp == std::string::npos) return -1;
    int st_dummy, ppid = -1;
    if (std::sscanf(s.c_str() + rp + 1, " %c %d", (char*)&st_dummy, &ppid) >= 2)
        return ppid;
    return -1;
}

// Is `pid` a Theia FC = a direct child of the supervisor? (osi-style scoping —
// IDSM watches the manifest-known process set, not arbitrary host processes.)
inline bool is_supervisor_child(int pid, int supervisor_pid) {
    return supervisor_pid > 0 && ppid_(pid) == supervisor_pid;
}

// SHA256 of a file via sha256sum (no openssl dep here; crypto FC owns real
// hashing — this is a cheap integrity spot-check). Returns lowercase hex or "".
inline std::string sha256_file(const std::string& path) {
    std::string out = run_("sha256sum " + path);
    auto sp = out.find(' ');
    if (sp == std::string::npos || sp == 0) return "";
    return out.substr(0, sp);
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
        auto listeners = scan_listeners();
        std::set<std::string> live;   // keys seen this scan (to expire reported_)

        for (const auto& l : listeners) {
            // Scope: only listeners owned by a Theia FC. Skips parse failures
            // (pid<=0) and every non-deployment host process.
            if (l.pid <= 0 || !is_supervisor_child(l.pid, supervisor_pid))
                continue;
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
            if (it->rfind("elfH:", 0) == 0 || live.count(it->substr(it->find(':') + 1)))
                ++it;
            else it = reported_.erase(it);
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
