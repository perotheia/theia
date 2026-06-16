// ptp_backend — queries the Linux time-sync stack (linuxptp / chrony) for the
// current discipline status. The FC NEVER disciplines the clock; it reads what
// the daemons report and normalizes it into a SyncSnapshot.
//
// v1 strategy (graceful degrade — the dev host has no linuxptp):
//   1. PTP:  `pmc` (ptp4l management client) → port state + offset + GM identity.
//   2. NTP:  `chronyc tracking` → sync state + offset (fallback source).
//   3. else: SYSTEM / UNAVAILABLE — the wall clock, undisciplined.
// Each query is a short popen of the daemon's own CLI (no library/data-path
// coupling); absence of the binary just degrades the source.
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
// TimeSource ordinals (.art): 0=SYSTEM 1=PTP 2=NTP.
enum TSource : int { T_SYSTEM = 0, T_PTP = 1, T_NTP = 2 };

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
    // Poll the time-sync stack. `iface` is the preferred PTP NIC (may be empty),
    // `prefer` is "ptp"|"ntp"|"system", `lock_offset_ns` is the LOCKED quality
    // gate. Tries the preferred source first, then degrades.
    static SyncSnapshot poll(const std::string& iface,
                             const std::string& prefer,
                             uint32_t lock_offset_ns) {
        if (prefer != "ntp" && prefer != "system") {
            SyncSnapshot s = poll_ptp_(iface, lock_offset_ns);
            if (s.state != S_UNAVAILABLE) return s;
        }
        if (prefer != "system") {
            SyncSnapshot s = poll_ntp_();
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

    // chrony via `chronyc tracking`: Leap/Stratum + "System time" offset.
    static SyncSnapshot poll_ntp_() {
        SyncSnapshot s;
        std::string t = run_("chronyc tracking");
        if (t.empty()) { s.state = S_UNAVAILABLE; return s; }
        s.source = T_NTP;
        // "System time : 0.000000123 seconds slow of NTP time"
        double secs = parse_double_(t, "System time");
        s.offset_ns = (long long)(secs * 1e9);
        // "Leap status : Normal" → synced.
        std::string leap = parse_after_(t, "Leap status");
        s.state = (leap.find("Normal") != std::string::npos) ? S_LOCKED : S_UNLOCKED;
        s.grandmaster = parse_after_(t, "Reference ID");
        s.message = "chrony leap=" + leap;
        return s;
    }

    static SyncSnapshot degrade_() {
        SyncSnapshot s;
        s.state   = S_UNAVAILABLE;
        s.source  = T_SYSTEM;
        s.message = "no ptp4l/chrony — system wall clock (undisciplined)";
        return s;
    }

    // --- tiny line parsers (the daemon CLIs are line-oriented) -------------
    static long long parse_ll_(const std::string& blob, const std::string& key) {
        auto p = blob.find(key);
        if (p == std::string::npos) return 0;
        return ::strtoll(blob.c_str() + p + key.size(), nullptr, 10);
    }
    static double parse_double_(const std::string& blob, const std::string& key) {
        auto p = blob.find(key);
        if (p == std::string::npos) return 0.0;
        // skip to first digit/sign after the key.
        const char* c = blob.c_str() + p + key.size();
        while (*c && (*c < '0' || *c > '9') && *c != '-' && *c != '.') ++c;
        return ::strtod(c, nullptr);
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
    static std::string parse_after_(const std::string& blob, const std::string& key) {
        auto p = blob.find(key);
        if (p == std::string::npos) return "";
        size_t colon = blob.find(':', p);
        if (colon == std::string::npos) return "";
        size_t eol = blob.find('\n', colon);
        std::string v = blob.substr(colon + 1, eol - colon - 1);
        size_t s = v.find_first_not_of(" \t");
        return s == std::string::npos ? "" : v.substr(s);
    }
};

}  // namespace ara::tsync
