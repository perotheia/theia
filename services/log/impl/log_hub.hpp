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
#include <cstring>
#include <ctime>
#include <deque>
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

namespace ara::log {

// One registered log subscriber. Persistent SEQPACKET client (connect once on
// subscribe, reuse per fan-out). A failed send => the consumer went away =>
// prune (connection-close demonitor).
struct LogSub {
    uint32_t tipc_type     = 0;
    uint32_t tipc_instance = 0;
    uint32_t level_min     = 0;   // best-effort coarse filter; 0 = all
    std::string tag_filter;       // "" = all
    std::shared_ptr<::theia::runtime::TipcClient> client;
};

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
    // service_id the consumer's receiver registers for — the djb2 of the
    // LogRecord type, so the fan-out frame dispatches on the subscriber side.
    static constexpr uint16_t kRecordServiceId =
        ::theia::runtime::hash_msg_type_("system_services_log_LogRecord");

    // Supervisor control node (SupervisorCtl) TIPC address — where
    // GetLoggerPolicy is served. Matches platform/supervisor/system/component.art.
    static constexpr uint32_t kSupervisorCtlType     = 0x80020001;
    static constexpr uint32_t kSupervisorCtlInstance = 0;

    static LogHub& instance() {
        static LogHub h;
        return h;
    }

    void set_capacity(std::size_t n) {
        std::lock_guard<std::mutex> lk(mu_);
        capacity_ = n ? n : 1;
        while (ring_.size() > capacity_) ring_.pop_front();
    }

    // LogDaemon control path: register a consumer. Connects, spills the ring
    // backlog (history, then follow), then live lines flow via the tailer's
    // fan-out. The FIRST subscriber starts the tailer thread; returns false if
    // the consumer is unreachable.
    bool subscribe(uint32_t type, uint32_t instance,
                   uint32_t level_min, std::string tag_filter) {
        auto client = std::make_shared<::theia::runtime::TipcClient>();
        if (!client->connect(type, instance)) {
            std::fprintf(stderr,
                "[log_hub] subscribe: cannot reach consumer {0x%08x,%u}\n",
                type, instance);
            return false;
        }
        LogSub sub;
        sub.tipc_type     = type;
        sub.tipc_instance = instance;
        sub.level_min     = level_min;
        sub.tag_filter    = std::move(tag_filter);
        sub.client        = std::move(client);

        bool start_tailer = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (const std::string& rec : ring_) {     // spill backlog
                if (!send_locked(sub, rec)) return false;  // died mid-spill
            }
            start_tailer = subs_.empty();   // first subscriber → spin up tailer
            subs_.push_back(std::move(sub));
            std::fprintf(stderr,
                "[log_hub] consumer {0x%08x,%u} attached (%zu backlog, %zu live)\n",
                type, instance, ring_.size(), subs_.size());
        }
        if (start_tailer) start_tailer_();
        return true;
    }

    // Called by LogStreamPump::do_loop on its own thread. Blocks tailing files
    // while there are subscribers; returns when the last one leaves (so the
    // runnable's loop idles). Re-entrant across start/stop cycles.
    void tail_loop() {
        // Fetch the per-node sinks once per tailer activation.
        std::vector<TailFile> files = fetch_and_open_();
        if (files.empty()) {
            std::fprintf(stderr,
                "[log_hub] tailer: no file: sinks to tail (policy empty or "
                "syslog-only) — idle\n");
        }
        while (run_tailer_.load() && have_subs_()) {
            bool any = false;
            for (TailFile& f : files) any |= drain_file_(f);
            if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        for (TailFile& f : files) if (f.fd >= 0) ::close(f.fd);
        std::fprintf(stderr, "[log_hub] tailer: stopped (%zu files closed)\n",
                     files.size());
    }

    // Whether the runnable's loop should keep calling tail_loop (i.e. there's
    // demand). LogStreamPump::do_loop polls this.
    bool tailer_wanted() const { return run_tailer_.load() && have_subs_(); }

private:
    LogHub() = default;

    bool have_subs_() const {
        std::lock_guard<std::mutex> lk(mu_);
        return !subs_.empty();
    }

    void start_tailer_() {
        // The runnable LogStreamPump owns the actual thread; we just flip the
        // run flag. do_loop() picks it up (polls tailer_wanted()). This keeps
        // the thread lifecycle with the node, not the hub.
        run_tailer_.store(true);
    }

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
        // Open each file: sink. (syslog entries are skipped in v1 — journald
        // ingest is the deferred v2 mode.)
        for (pb_size_t i = 0; i < pol.entries_count; ++i) {
            const auto& e = pol.entries[i];
            if (std::strcmp(e.sink, "file") != 0) continue;
            TailFile f;
            f.node = e.node;
            f.tag  = e.tag[0] ? e.tag : e.node;
            f.path = e.path;
            // Open + seek to END (follow new lines only; the ring has backlog).
            f.fd = ::open(f.path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (f.fd >= 0) ::lseek(f.fd, 0, SEEK_END);
            else std::fprintf(stderr,
                     "[log_hub] tailer: cannot open %s (skipping)\n",
                     f.path.c_str());
            out.push_back(std::move(f));
        }
        std::fprintf(stderr,
            "[log_hub] tailer: policy machine_sink=%s, tailing %zu file sinks\n",
            pol.machine_sink, out.size());
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

        uint8_t wire[BUFSIZ + 256];
        pb_ostream_t os = pb_ostream_from_buffer(wire, sizeof(wire));
        if (!pb_encode(&os, system_services_log_LogRecord_fields, &rec)) return;
        std::string bytes(reinterpret_cast<const char*>(wire), os.bytes_written);

        std::lock_guard<std::mutex> lk(mu_);
        ring_.push_back(bytes);
        while (ring_.size() > capacity_) ring_.pop_front();
        auto it = subs_.begin();
        while (it != subs_.end()) {
            if (!send_locked(*it, ring_.back())) {
                std::fprintf(stderr,
                    "[log_hub] consumer {0x%08x,%u} gone — pruning\n",
                    it->tipc_type, it->tipc_instance);
                it = subs_.erase(it);
            } else {
                ++it;
            }
        }
        // Last consumer left mid-drain → ask the tailer to wind down.
        if (subs_.empty()) run_tailer_.store(false);
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

    // Frame raw record bytes as a GEN_CAST and send to one subscriber.
    // Caller holds mu_.
    bool send_locked(LogSub& sub, const std::string& wire) {
        if (!sub.client || !sub.client->is_open()) return false;
        ::theia::runtime::TheiaMsgHeader hdr{};
        hdr.bus_type           = ::theia::runtime::kBusTypeRpc;
        hdr.msg_type           = ::theia::runtime::kMsgGenCast;
        hdr.proto_len          = static_cast<uint16_t>(wire.size());
        hdr.rpc.service_id     = kRecordServiceId;
        hdr.rpc.method_id      = 0;
        hdr.rpc.correlation_id = 0;
        return sub.client->send_frame(
            hdr, reinterpret_cast<const uint8_t*>(wire.data()),
            static_cast<uint16_t>(wire.size()));
    }

    mutable std::mutex mu_;
    std::deque<std::string> ring_;
    std::size_t capacity_ = 4096;
    std::vector<LogSub> subs_;
    std::atomic<bool> run_tailer_{false};
};

}  // namespace ara::log
