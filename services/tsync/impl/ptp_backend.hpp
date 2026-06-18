// ptp_backend — queries linuxptp (ptp4l) for the current PTP discipline status.
// This is the DISTRIBUTION side: on the compute (slave) board ptp4l locks to the
// central grandmaster, and this reports that lock. The ACQUISITION side (GNSS) is
// gps_backend.hpp. NTP/chrony is gone — the time source is GPS, not NTP.
//
// v1 strategy (graceful degrade — the dev host has no linuxptp):
//   1. PTP:  `pmc` (ptp4l management client) → port state + offset + GM identity.
//   2. else: SYSTEM / UNAVAILABLE — the wall clock, undisciplined.
// The query is a short popen of ptp4l's own CLI (no library/data-path coupling);
// absence of the binary just degrades the source.
//
// SyncState / TimeSource ordinals MUST match the .art enums.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace ara::tsync {

// SyncState ordinals (.art): 0=UNAVAILABLE 1=UNLOCKED 2=HOLDOVER 3=LOCKED.
enum SState : int { S_UNAVAILABLE = 0, S_UNLOCKED = 1, S_HOLDOVER = 2, S_LOCKED = 3 };
// TimeSource ordinals (.art): 0=SYSTEM 1=PTP 2=GPS.
enum TSource : int { T_SYSTEM = 0, T_PTP = 1, T_GPS = 2 };

struct SyncSnapshot {
    int         state  = S_UNAVAILABLE;
    int         source = T_SYSTEM;
    long long   offset_ns = 0;
    std::string grandmaster;
    std::string interface;
    std::string message;
};

class PtpBackend {
public:
    // Poll ptp4l. `iface` is the preferred PTP NIC (may be empty), `prefer` is
    // "system" to skip PTP entirely (e.g. central, where GPS is the source),
    // else PTP is tried and we degrade to the wall clock if there's no daemon.
    static SyncSnapshot poll(const std::string& iface,
                             const std::string& prefer,
                             uint32_t lock_offset_ns) {
        if (prefer != "system") {
            SyncSnapshot s = poll_ptp_(iface, lock_offset_ns);
            if (s.state != S_UNAVAILABLE) return s;
        }
        return degrade_();
    }

private:
    // Run a command, capture stdout (best-effort, capped). Empty on failure.
    static std::string run_(const std::string& cmd) {
        FILE* p = ::popen((cmd + " 2>/dev/null").c_str(), "r");
        if (!p) return "";
        std::string out; char buf[512]; size_t n;
        while ((n = ::fread(buf, 1, sizeof(buf), p)) > 0) {
            out.append(buf, n);
            if (out.size() > 8192) break;
        }
        ::pclose(p);
        return out;
    }

    // ptp4l via `pmc`: query the local port's PORT_DATA_SET (state) +
    // CURRENT_DATA_SET (offsetFromMaster) + PARENT_DATA_SET (GM identity).
    static SyncSnapshot poll_ptp_(const std::string& iface,
                                  uint32_t lock_offset_ns) {
        SyncSnapshot s;
        // pmc speaks to a running ptp4l over its UDS. No daemon → empty.
        std::string cur = run_("pmc -u -b 0 'GET CURRENT_DATA_SET'");
        if (cur.empty()) { s.state = S_UNAVAILABLE; return s; }
        s.source    = T_PTP;
        s.interface = iface;
        // offsetFromMaster (ns) — parse the line.
        long long off = parse_ll_(cur, "offsetFromMaster");
        s.offset_ns = off;
        std::string port = run_("pmc -u -b 0 'GET PORT_DATA_SET'");
        std::string pstate = parse_word_(port, "portState");
        std::string parent = run_("pmc -u -b 0 'GET PARENT_DATA_SET'");
        s.grandmaster = parse_word_(parent, "grandmasterIdentity");
        // SLAVE + offset within gate = LOCKED; SLAVE but loose = UNLOCKED;
        // MASTER/holdover heuristics kept simple for v1.
        long long mag = off < 0 ? -off : off;
        if (pstate == "SLAVE")
            s.state = (mag <= (long long)lock_offset_ns) ? S_LOCKED : S_UNLOCKED;
        else if (pstate == "UNCALIBRATED" || pstate == "LISTENING")
            s.state = S_UNLOCKED;
        else
            s.state = S_HOLDOVER;
        s.message = "ptp4l portState=" + pstate;
        return s;
    }

    static SyncSnapshot degrade_() {
        SyncSnapshot s;
        s.state   = S_UNAVAILABLE;
        s.source  = T_SYSTEM;
        s.message = "no ptp4l — system wall clock (undisciplined)";
        return s;
    }

    // --- tiny line parsers (the daemon CLIs are line-oriented) -------------
    static long long parse_ll_(const std::string& blob, const std::string& key) {
        auto p = blob.find(key);
        if (p == std::string::npos) return 0;
        return ::strtoll(blob.c_str() + p + key.size(), nullptr, 10);
    }
    static std::string parse_word_(const std::string& blob, const std::string& key) {
        auto p = blob.find(key);
        if (p == std::string::npos) return "";
        size_t i = p + key.size();
        while (i < blob.size() && (blob[i] == ' ' || blob[i] == '\t')) ++i;
        size_t j = i;
        while (j < blob.size() && blob[j] != ' ' && blob[j] != '\n' &&
               blob[j] != '\t') ++j;
        return blob.substr(i, j - i);
    }
};

}  // namespace ara::tsync
