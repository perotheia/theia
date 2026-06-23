// LogHub — the process-global, mutex-guarded LOG ring + subscriber registry +
// on-demand file tailer shared by the two log-stream nodes:
//
//   LogStreamPump (runnable)  drives tail_loop()   — tails files, fan-out
//   LogDaemon     (atomic)    calls subscribe(...)  — register a consumer
//
// The LOG analogue of TraceHub (impl/trace_hub.hpp). Two differences from the
// trace side:
//
//   1. The PRODUCER is files on disk, not a TIPC submitter. The hub tails every
//      node's log file (the exact paths the supervisor's GetLoggerPolicy
//      returns) and ENCODES a LogRecord per new line — so unlike the trace hub
//      (which forwards opaque bytes), this hub builds the wire record itself.
//   2. The tailer is LAZY + reference-counted: it starts on the FIRST
//      subscriber (queries the supervisor, opens the files) and stops on the
//      LAST unsubscribe — no file I/O when nobody's watching (per design).
//
// A process-global singleton so the two node threads reach the SAME hub without
// main.cc wiring (main.cc is generated). See docs/tasks/TODO/log-logcat.md.

#pragma once

#define _GNU_SOURCE 1   // strptime / timegm

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <pb_encode.h>
#include <pb_decode.h>
#include "system/services/log/log.pb.h"
#include "system/supervisor/supervisor.pb.h"   // GetLoggerPolicy req/reply types

#include "NodeRef.hh"        // theia::runtime::TipcClient
#include "RemoteCodec.hh"    // service_id for the fan-out frame
#include "TheiaMsgHeader.hh"
#include "PgClient.hh"       // OTP pg:monitor — membership-driven fan-out + tailer
#include "lib/log_codecs.hh" // RemoteCodec<system_services_log_LogRecord>

namespace ara::log {

using LogRecordT = system_services_log_LogRecord;

// One tailed log file: the node it belongs to + its open offset.
struct TailFile {
    std::string node;   // the supervised worker name (exact, from supervisor)
    std::string tag;    // the tag the lines carry (node name today)
    std::string path;   // <dir>/<node>.log
    int         fd = -1;
    std::string carry;  // partial last line not yet terminated by '\n'
};

class LogHub {
public:
    // Supervisor control node (SupervisorCtl) TIPC address — where
    // GetLoggerPolicy is served. Matches platform/supervisor/system/component.art.
    static constexpr uint32_t kSupervisorCtlType     = 0x80020001;
    static constexpr uint32_t kSupervisorCtlInstance = 0;

    static LogHub& instance() {
        static LogHub h;
        return h;
    }

    // OTP pg:monitor — the pump calls this ONCE (with its own bound addr) to
    // start watching the LogRecord group. Membership now DRIVES the lazy tailer:
    // pg_watch'ing means the supervisor pushes us PgMembership on every consumer
    // join/leave, and tailer_wanted() reads the member count. `self_t/self_i` is
    // the pump's bound addr — where the supervisor casts PgMembership.
    void watch_group(const std::string& node_name,
                     ::theia::runtime::NodeBinding* binding,
                     uint32_t self_t, uint32_t self_i) {
        pg_.attach(node_name, binding);
        watch_self_t_ = self_t;
        watch_self_i_ = self_i;
        // on_change: wake the tailer thread (a consumer just joined → start, or
        // left → maybe stop). The pump's do_loop polls tailer_wanted() anyway;
        // this just makes the transition prompt.
        pg_.watch<LogRecordT>(self_t, self_i, [] { /* poll-driven */ });
    }

    // Called by LogStreamPump::do_loop on its own thread. Blocks tailing files
    // while the LogRecord group has members; returns when the last one leaves (so
    // the runnable's loop idles). Re-entrant across start/stop cycles.
    void tail_loop() {
        // Fetch the per-node sinks once per tailer activation. Populates `files`
        // (file: sinks) and journal_tags_ (syslog sinks).
        std::vector<TailFile> files = fetch_and_open_();
        // v2: if any node logs to syslog, follow journald for its tags. One
        // `journalctl -f -o json -t <tag>...` subprocess, remixed by journald.
        FILE* journal = journal_tags_.empty() ? nullptr : open_journal_();
        std::string jbuf;   // partial JSON line carry across reads
        int jfd = journal ? ::fileno(journal) : -1;
        if (jfd >= 0) ::fcntl(jfd, F_SETFL, O_NONBLOCK);

        if (files.empty() && !journal) {
            std::fprintf(stderr,
                "[log_hub] tailer: no file:/syslog sinks to follow — idle\n");
        }
        while (have_subs_()) {
            bool any = false;
            for (TailFile& f : files) any |= drain_file_(f);
            if (jfd >= 0) any |= drain_journal_(jfd, jbuf);
            if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        for (TailFile& f : files) if (f.fd >= 0) ::close(f.fd);
        if (journal) ::pclose(journal);
        std::fprintf(stderr,
            "[log_hub] tailer: stopped (%zu files, %zu syslog tags closed)\n",
            files.size(), journal_tags_.size());
    }

    // Whether the runnable's loop should keep calling tail_loop (i.e. the
    // LogRecord group has ≥1 member). LogStreamPump::do_loop polls this — file
    // I/O happens ONLY while someone is logcat'ing. The OTP-pg analogue of the
    // old subscriber refcount; now the supervisor's PgMembership IS the count.
    bool tailer_wanted() { return have_subs_(); }

private:
    LogHub() = default;

    bool have_subs_() { return !pg_.members<LogRecordT>().empty(); }

    // ---- supervisor GetLoggerPolicy (hand-rolled SEQPACKET call) -----------
    //
    // The pump can't reverse a node's exact log path from the machine policy,
    // so it asks the supervisor (which authored every THEIA_LOGGER). One-shot
    // SEQPACKET call with a generous reply buffer (the LoggerPolicy carries one
    // entry per supervised node).
    std::vector<TailFile> fetch_and_open_() {
        std::vector<TailFile> out;
        ::theia::runtime::TipcClient cli;
        if (!cli.connect(kSupervisorCtlType, kSupervisorCtlInstance,
                         /*total_timeout_ms=*/3000)) {
            std::fprintf(stderr,
                "[log_hub] tailer: cannot reach supervisor ctl 0x%08x — "
                "no logger policy\n", kSupervisorCtlType);
            return out;
        }
        // Encode an empty GetLoggerPolicyRequest.
        system_supervisor_GetLoggerPolicyRequest req =
            system_supervisor_GetLoggerPolicyRequest_init_zero;
        uint8_t reqbuf[16];
        pb_ostream_t os = pb_ostream_from_buffer(reqbuf, sizeof(reqbuf));
        if (!pb_encode(&os, system_supervisor_GetLoggerPolicyRequest_fields,
                       &req)) {
            std::fprintf(stderr, "[log_hub] tailer: encode req failed\n");
            return out;
        }
        ::theia::runtime::TheiaMsgHeader hdr{};
        hdr.bus_type           = ::theia::runtime::kBusTypeRpc;
        hdr.msg_type           = ::theia::runtime::kMsgGenCall;
        hdr.proto_len          = static_cast<uint16_t>(os.bytes_written);
        hdr.rpc.service_id     =
            ::theia::runtime::hash_msg_type_(
                "system_supervisor_GetLoggerPolicyRequest");
        hdr.rpc.method_id      = 0;
        hdr.rpc.correlation_id = 1;
        if (!cli.send_frame(hdr, reqbuf,
                            static_cast<uint16_t>(os.bytes_written))) {
            std::fprintf(stderr, "[log_hub] tailer: send GetLoggerPolicy failed\n");
            return out;
        }
        // Read the reply frame: [TheiaMsgHeader][LoggerPolicy proto-wire].
        // LoggerPolicy is large (entries[64] × LoggerEntry); use a big buffer.
        std::vector<uint8_t> rbuf(64 * 1024);
        ssize_t r = ::recv(cli.fd(), rbuf.data(), rbuf.size(), 0);
        if (r < static_cast<ssize_t>(
                sizeof(::theia::runtime::TheiaMsgHeader))) {
            std::fprintf(stderr, "[log_hub] tailer: short reply (%zd)\n", r);
            return out;
        }
        ::theia::runtime::TheiaMsgHeader rh{};
        std::memcpy(&rh, rbuf.data(), sizeof(rh));
        const uint8_t* body = rbuf.data() + sizeof(rh);
        size_t avail = static_cast<size_t>(r) - sizeof(rh);
        size_t plen  = rh.proto_len <= avail ? rh.proto_len : avail;

        system_supervisor_LoggerPolicy pol =
            system_supervisor_LoggerPolicy_init_zero;
        pb_istream_t is = pb_istream_from_buffer(body, plen);
        if (!pb_decode(&is, system_supervisor_LoggerPolicy_fields, &pol)) {
            std::fprintf(stderr, "[log_hub] tailer: decode LoggerPolicy failed\n");
            return out;
        }
        // Split the entries by sink: file: sinks are tailed directly; syslog
        // sinks are followed via journald (v2). A machine can mix both (a
        // per-process override), so we handle each entry by its own sink.
        journal_tags_.clear();
        journal_node_by_tag_.clear();
        for (pb_size_t i = 0; i < pol.entries_count; ++i) {
            const auto& e = pol.entries[i];
            const std::string tag = e.tag[0] ? e.tag : e.node;
            if (std::strcmp(e.sink, "file") == 0) {
                TailFile f;
                f.node = e.node;
                f.tag  = tag;
                f.path = e.path;
                // Open + seek to END (follow new lines; the ring has backlog).
                f.fd = ::open(f.path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
                if (f.fd >= 0) ::lseek(f.fd, 0, SEEK_END);
                else std::fprintf(stderr,
                         "[log_hub] tailer: cannot open %s (skipping)\n",
                         f.path.c_str());
                out.push_back(std::move(f));
            } else if (std::strcmp(e.sink, "syslog") == 0) {
                journal_tags_.push_back(tag);
                journal_node_by_tag_[tag] = e.node;
            }
            // stdio/null sinks have no tailable stream — skipped.
        }
        std::fprintf(stderr,
            "[log_hub] tailer: policy machine_sink=%s, %zu file sinks + "
            "%zu syslog tags\n",
            pol.machine_sink, out.size(), journal_tags_.size());
        return out;
    }

    // Read whatever's appended to one file since last drain; split on '\n',
    // parse + fan out each complete line. Returns true if any bytes were read.
    bool drain_file_(TailFile& f) {
        if (f.fd < 0) return false;
        char buf[BUFSIZ];
        bool any = false;
        for (;;) {
            ssize_t n = ::read(f.fd, buf, sizeof(buf));
            if (n <= 0) break;
            any = true;
            f.carry.append(buf, static_cast<size_t>(n));
            size_t nl;
            while ((nl = f.carry.find('\n')) != std::string::npos) {
                std::string line = f.carry.substr(0, nl);
                f.carry.erase(0, nl + 1);
                emit_line_(f, line);
            }
            // Guard a runaway partial line (no newline) at BUFSIZ cap.
            if (f.carry.size() > BUFSIZ) {
                emit_line_(f, f.carry.substr(0, BUFSIZ));
                f.carry.clear();
            }
        }
        return any;
    }

    // Parse "<ISO8601Z> [<LEVEL>] <msg>" (FileLogger format; Logger.cc) into a
    // LogRecord, encode it, push to ring + fan out. Lines that don't match the
    // shape still hose as INFO with the whole line as the message.
    void emit_line_(const TailFile& f, const std::string& line) {
        system_services_log_LogRecord rec = system_services_log_LogRecord_init_zero;
        std::snprintf(rec.node, sizeof(rec.node), "%s", f.node.c_str());
        std::snprintf(rec.tag,  sizeof(rec.tag),  "%s", f.tag.c_str());
        rec.level = parse_level_(line);
        rec.ts_ns = parse_ts_ns_(line);
        // `line` carries just the MESSAGE: the ts + [LEVEL] prefix is parsed out
        // into ts_ns/level above, so we strip "<ISO8601Z> [<LEVEL>] " here — the
        // consumer re-renders ts + level from the record fields and would
        // otherwise double-print them (FileLogger's own "<ts> [LEVEL] msg").
        const std::string msg = strip_log_prefix_(line);
        std::snprintf(rec.line, sizeof(rec.line), "%s", msg.c_str());
        encode_and_fanout_(rec);
    }

    // Encode a LogRecord → PG fan out: cast to EACH watched member of the
    // LogRecord group (OTP `[Pid ! Msg || Pid <- pg:get_members]`). Shared by the
    // file: tailer (emit_line_) and the syslog/journald tailer. No ring (logcat
    // is a live tail; history is in the files/journald).
    void encode_and_fanout_(const system_services_log_LogRecord& rec) {
        uint8_t wire[BUFSIZ + 256];
        pb_ostream_t os = pb_ostream_from_buffer(wire, sizeof(wire));
        if (!pb_encode(&os, system_services_log_LogRecord_fields, &rec)) return;
        pg_.broadcast_members<LogRecordT>(
            wire, static_cast<uint16_t>(os.bytes_written));
    }

    // Strip the FileLogger prefix "<ISO8601Z> [<LEVEL>] " so `line` holds only
    // the message (the ts + level already ride the record's ts_ns/level fields).
    // Only strips when the line matches the shape — a non-conforming line is
    // returned verbatim (it hosed as INFO with ts_ns=0, so keep its full text).
    static std::string strip_log_prefix_(const std::string& line) {
        // Expect "...Z [LEVEL] msg": find the FIRST "] " at/after the '[' that
        // opens the level tag, and return what follows.
        auto lb = line.find('[');
        if (lb == std::string::npos) return line;
        // The level tag is a 5-char field then ']' — i.e. "[XXXXX]". Require a
        // 'Z' (ISO ts terminator) before the '[' so we only strip OUR prefix,
        // not a '[' that's part of an arbitrary message.
        if (line.find('Z') == std::string::npos || line.find('Z') > lb)
            return line;
        auto close = line.find("] ", lb);
        if (close == std::string::npos) return line;
        return line.substr(close + 2);
    }

    // "[INFO ]" / "[WARN ]" / "[ERROR]" / "[DEBUG]" / "[TRACE]" → LogLevel.
    static system_services_log_LogLevel parse_level_(const std::string& line) {
        auto lb = line.find('[');
        if (lb != std::string::npos && lb + 6 <= line.size()) {
            std::string t = line.substr(lb + 1, 5);
            if (t.rfind("ERROR", 0) == 0) return system_services_log_LogLevel_LogLevel_ERROR;
            if (t.rfind("WARN", 0)  == 0) return system_services_log_LogLevel_LogLevel_WARNING;
            if (t.rfind("DEBUG", 0) == 0) return system_services_log_LogLevel_LogLevel_DEBUG;
            if (t.rfind("TRACE", 0) == 0) return system_services_log_LogLevel_LogLevel_VERBOSE;
            if (t.rfind("FATAL", 0) == 0) return system_services_log_LogLevel_LogLevel_FATAL;
        }
        return system_services_log_LogLevel_LogLevel_INFO;
    }

    // Parse the leading ISO8601 "YYYY-MM-DDTHH:MM:SS.mmmZ" → epoch ns. The .mmm
    // (FileLogger now writes millis) is parsed too so logcat shows sub-second
    // time like tracecat — strptime stops at %S, so we read the fraction by
    // hand. 0 if the stamp is absent/malformed.
    static uint64_t parse_ts_ns_(const std::string& line) {
        struct tm tm{};
        const char* rest = ::strptime(line.c_str(), "%Y-%m-%dT%H:%M:%S", &tm);
        if (rest == nullptr) return 0;
        time_t t = ::timegm(&tm);
        if (t == static_cast<time_t>(-1)) return 0;
        uint64_t ns = static_cast<uint64_t>(t) * 1000000000ull;
        // Optional ".mmm" fraction right after the seconds.
        if (*rest == '.') {
            uint64_t ms = 0; int digits = 0;
            for (const char* p = rest + 1; *p >= '0' && *p <= '9' && digits < 3;
                 ++p, ++digits) {
                ms = ms * 10 + static_cast<uint64_t>(*p - '0');
            }
            // Pad to full millis if fewer than 3 digits were present.
            while (digits++ < 3) ms *= 10;
            ns += ms * 1000000ull;
        }
        return ns;
    }

    // ---- syslog (journald) ingest — v2 -------------------------------------
    //
    // When a node logs to syslog, SyslogLogger uses kNodeName as the ident, so
    // journald indexes its lines under SYSLOG_IDENTIFIER=<tag>. We follow them
    // with `journalctl -f -o json -t <tag>...` — journald is already remixed
    // across nodes, so we just re-frame each JSON entry into a LogRecord.
    FILE* open_journal_() {
        // Build: journalctl -f -n 0 -o json -t <tag1> -t <tag2> ...
        // -n 0 = no history (the ring already holds backlog from file: peers;
        // for a pure-syslog machine the consumer starts live, like adb -f).
        std::string cmd = "journalctl -f -n 0 -o json --no-pager";
        for (const std::string& tag : journal_tags_) {
            cmd += " -t '";
            // Escape any single-quote in the tag (defensive; tags are node
            // names, but never trust a string in a shell command).
            for (char c : tag) { if (c == '\'') cmd += "'\\''"; else cmd += c; }
            cmd += '\'';
        }
        cmd += " 2>/dev/null";
        FILE* p = ::popen(cmd.c_str(), "r");
        if (!p) {
            std::fprintf(stderr,
                "[log_hub] tailer: cannot spawn journalctl (syslog ingest off)\n");
            return nullptr;
        }
        std::fprintf(stderr,
            "[log_hub] tailer: following journald for %zu syslog tag(s)\n",
            journal_tags_.size());
        return p;
    }

    // Read available bytes from the journalctl pipe (non-blocking), split on
    // '\n', parse each JSON entry → LogRecord. Returns true if any was read.
    bool drain_journal_(int jfd, std::string& carry) {
        char buf[BUFSIZ];
        bool any = false;
        for (;;) {
            ssize_t n = ::read(jfd, buf, sizeof(buf));
            if (n <= 0) break;          // EAGAIN (no data) or EOF — done for now
            any = true;
            carry.append(buf, static_cast<size_t>(n));
            size_t nl;
            while ((nl = carry.find('\n')) != std::string::npos) {
                std::string jline = carry.substr(0, nl);
                carry.erase(0, nl + 1);
                emit_journal_entry_(jline);
            }
            if (carry.size() > 4 * BUFSIZ) carry.clear();   // runaway guard
        }
        return any;
    }

    // Parse one journald JSON entry {SYSLOG_IDENTIFIER, MESSAGE, PRIORITY,
    // __REALTIME_TIMESTAMP} into a LogRecord and fan it out. Minimal JSON field
    // extraction (no JSON lib dep) — the keys are fixed and journald's -o json
    // emits flat string values.
    void emit_journal_entry_(const std::string& jline) {
        const std::string tag = json_str_(jline, "SYSLOG_IDENTIFIER");
        const std::string msg = json_str_(jline, "MESSAGE");
        if (msg.empty() && tag.empty()) return;
        // node: map the tag back to the supervised node name (GetLoggerPolicy
        // gave us tag→node); fall back to the tag itself.
        auto it = journal_node_by_tag_.find(tag);
        const std::string node = it != journal_node_by_tag_.end() ? it->second : tag;

        system_services_log_LogRecord rec = system_services_log_LogRecord_init_zero;
        std::snprintf(rec.node, sizeof(rec.node), "%s", node.c_str());
        std::snprintf(rec.tag,  sizeof(rec.tag),  "%s", tag.c_str());
        rec.level = syslog_prio_to_level_(json_str_(jline, "PRIORITY"));
        // __REALTIME_TIMESTAMP is epoch MICROSECONDS.
        const std::string us = json_str_(jline, "__REALTIME_TIMESTAMP");
        rec.ts_ns = us.empty() ? 0
                  : static_cast<uint64_t>(std::strtoull(us.c_str(), nullptr, 10))
                        * 1000ull;
        std::snprintf(rec.line, sizeof(rec.line), "%s", msg.c_str());
        encode_and_fanout_(rec);
    }

    // Extract a flat string value for `key` from a journald JSON object. journald
    // -o json escapes only \" and \\ in MESSAGE; good enough for our fixed keys.
    static std::string json_str_(const std::string& obj, const char* key) {
        std::string needle = std::string("\"") + key + "\":";
        auto k = obj.find(needle);
        if (k == std::string::npos) return "";
        auto v = obj.find_first_not_of(' ', k + needle.size());
        if (v == std::string::npos) return "";
        if (obj[v] != '"') {            // unquoted (number/null) — read to , or }
            auto e = obj.find_first_of(",}", v);
            return obj.substr(v, e == std::string::npos ? std::string::npos : e - v);
        }
        // Quoted string — unescape \" and \\.
        std::string out;
        for (size_t i = v + 1; i < obj.size(); ++i) {
            char c = obj[i];
            if (c == '\\' && i + 1 < obj.size()) { out += obj[++i]; continue; }
            if (c == '"') break;
            out += c;
        }
        return out;
    }

    // syslog PRIORITY (0=emerg … 7=debug) → LogLevel. Maps the 8 syslog levels
    // onto our 6 (emerg/alert/crit → FATAL; err → ERROR; warning → WARNING;
    // notice/info → INFO; debug → DEBUG).
    static system_services_log_LogLevel syslog_prio_to_level_(const std::string& p) {
        int pr = p.empty() ? 6 : std::atoi(p.c_str());
        if (pr <= 2) return system_services_log_LogLevel_LogLevel_FATAL;
        if (pr == 3) return system_services_log_LogLevel_LogLevel_ERROR;
        if (pr == 4) return system_services_log_LogLevel_LogLevel_WARNING;
        if (pr <= 6) return system_services_log_LogLevel_LogLevel_INFO;
        return system_services_log_LogLevel_LogLevel_DEBUG;
    }

    // The PG client: pg_watch'es the LogRecord group (membership drives the
    // tailer) + broadcast_members fans each line to the watched consumers. The
    // OTP-pg replacement for the old subscriber registry + ring.
    ::theia::runtime::PgClient pg_;
    uint32_t watch_self_t_{0};
    uint32_t watch_self_i_{0};

    // syslog-mode state, (re)populated by fetch_and_open_ each tailer
    // activation: the SYSLOG_IDENTIFIER tags to follow via journald + their
    // tag→node mapping (only touched on the single tailer thread).
    std::vector<std::string>                     journal_tags_;
    std::map<std::string, std::string>           journal_node_by_tag_;
};

}  // namespace ara::log
