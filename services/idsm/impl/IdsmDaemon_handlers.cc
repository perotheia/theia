// User handler bodies for IdsmDaemon — the IDS-manager node.
//
// HAND-OWNED (gen-app emits this once, then skips it without --force).
//
// IdsmDaemon manages the eBPF detector, drains its ring buffer on a tick,
// normalizes each detection into a TraceRecord (kind=SECURITY), and SUBMITS it
// to the trace firehose (the same TIPC SOCK_DGRAM path the runtime Tracer uses,
// to log[trace] @ 0x80010013) so tdb/observer/GUI see security events inline
// with dispatcher traces. Detections at/above escalate_severity also bump the
// escalation counter (the PHM/SM escalation cast is a future hook). Never in the
// packet path — the kernel detects, this manages + ingests.
//
// Graceful-degrades: no eBPF backend (the dev host) → IDS_UNAVAILABLE, but a
// configured mock_event_path keeps the full pipeline exercisable.

#include "lib/IdsmDaemon.hh"
#include "impl/ids_backend.hpp"

#include "TimerService.hh"   // post_info / send_after / process_timers
#include "Tracer.hh"         // TraceSubmitter — the firehose egress

#include "system/services/log/log.pb.h"   // TraceKind_SECURITY ordinal

#include <pb_decode.h>
#include <pb_encode.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <unistd.h>   // getppid

namespace ara::idsm {

namespace {

uint64_t now_ns_() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

// Minimal proto3 wire helpers — the TraceRecord's string/bytes fields are
// pb_callback_t in nanopb (the trace hub forwards records VERBATIM and never
// decodes, so log.options deliberately leaves them unbounded). So we HAND-ENCODE
// the record, exactly as the runtime Tracer does (Tracer.hh), rather than pull
// the callback machinery in.
static void pb_varint_(std::string& o, uint64_t v) {
    while (v >= 0x80) { o += char((v & 0x7F) | 0x80); v >>= 7; }
    o += char(v);
}
static void pb_tag_(std::string& o, uint32_t field, uint32_t wire) {
    pb_varint_(o, (uint64_t(field) << 3) | wire);
}
static void pb_str_(std::string& o, uint32_t field, const std::string& s) {
    if (s.empty()) return;
    pb_tag_(o, field, 2); pb_varint_(o, s.size()); o += s;
}
static void pb_vfield_(std::string& o, uint32_t field, uint64_t v) {
    if (!v) return;
    pb_tag_(o, field, 0); pb_varint_(o, v);
}

// Normalize one detection into a TraceRecord(kind=SECURITY) wire blob and submit
// it to the firehose. Field layout MATCHES Tracer.hh / the .art TraceRecord:
//   1=node_name 2=dst 3=msg_type 4=corr_id 5=ts_ns 6=kind 7=payload.
// node_name="idsm" (the producer), msg_type=<signature> (tdb's type column),
// dst=<target>, corr_id=severity, payload="src -> dst sev=N" (a readable blob).
void submit_detection_(const DetectionEv& ev) {
    std::string rec;
    pb_str_(rec, 1, "idsm");
    pb_str_(rec, 2, ev.dst);
    pb_str_(rec, 3, ev.signature);
    pb_vfield_(rec, 4, ev.severity);                       // severity in corr_id
    pb_vfield_(rec, 5, ev.ts_ns ? ev.ts_ns : now_ns_());
    pb_vfield_(rec, 6, system_services_log_TraceKind_TraceKind_SECURITY);
    std::string p = ev.src + " -> " + ev.dst + " sev=" + std::to_string(ev.severity);
    if (!p.empty()) { pb_tag_(rec, 7, 2); pb_varint_(rec, p.size()); rec += p; }
    ::theia::runtime::TraceSubmitter::instance().submit(rec);
}

// Apply config to the backend: (re)open the source with the current paths.
void reopen_backend_(IdsmDaemonState& s) {
    s.state = s.backend.open(s.bpf_object_path, s.ringbuf_map, s.mock_event_path);
}

// Build + broadcast the status snapshot.
void broadcast_status_(IdsmDaemon& self, IdsmDaemonState& s) {
    IdsStatusMsg msg = system_services_idsm_IdsStatusMsg_init_zero;
    msg.state = static_cast<system_services_idsm_IdsState>(s.state);
    msg.on_ebpf = s.backend.on_ebpf();
    msg.events_total = s.events_total;
    msg.escalated_total = s.escalated_total;
    std::strncpy(msg.last_signature, s.last_signature.c_str(),
                 sizeof(msg.last_signature) - 1);
    msg.ts_ns = now_ns_();
    self.broadcast_broadcast_status(msg);
}

// Spill a batch of detections to the firehose + escalate the severe ones. Shared
// by the eBPF/mock backend poll AND the userspace proc scan. Returns true if any
// were emitted (so the caller re-broadcasts status).
bool process_detections_(IdsmDaemon& self, IdsmDaemonState& s,
                         const std::vector<DetectionEv>& events) {
    for (const auto& ev : events) {
        submit_detection_(ev);                 // → trace firehose (kind=SECURITY)
        s.events_total++;
        s.last_signature = ev.signature;
        if (s.escalate_severity && ev.severity >= s.escalate_severity) {
            s.escalated_total++;
            // PHM/SM escalation hook: a critical detection is a health event. The
            // cast to PHM/SM lands here once those receiver ports exist; for now
            // the count + a WARN make it observable.
            self.log().warn("IDS escalation: '" + ev.signature + "' sev=" +
                            std::to_string(ev.severity) + " (" + ev.src + " -> " +
                            ev.dst + ")");
        }
    }
    return !events.empty();
}

// Apply the ProcDetector config (Cat A/C/D/H allow-lists) + seed the known-FC
// set so it can tell a real-FC-on-wrong-port (Cat A) from a rogue listener (C).
void apply_proc_config_(IdsmDaemonState& s) {
    s.proc.configure(s.expected_listeners, s.grpc_servers, s.elf_digests);
    // The platform FCs (the manifest's process set). Hardcoded here = the
    // services + demo apps the supervisor forks; a manifest reader can replace
    // this. Used only to classify A-vs-C; not a security gate.
    s.proc.set_known_fcs({"com", "crypto", "log", "nm", "osi", "per", "sm",
                          "tsync", "ucm", "shwa", "fw", "idsm",
                          "p1", "p2", "p3", "p4"});
}

}  // namespace

// init: open the detector + the userspace proc sensor + kick off the drain loop.
void IdsmDaemon::init(IdsmDaemonState& s) {
    reopen_backend_(s);
    s.supervisor_pid = ::getppid();   // FC processes are the supervisor's children
    apply_proc_config_(s);
    log().info(std::string("idsm up — IDS manager (eBPF ring-buffer + userspace "
        "ss/proc → SECURITY firehose); state=") + std::to_string(s.state) +
        " on_ebpf=" + (s.backend.on_ebpf() ? "yes" : "no") +
        " proc_scan=" + (s.proc_scan ? "on" : "off"));
    broadcast_status_(*this, s);
    ::theia::runtime::post_info(*this, "drain");
}

// handle_info "drain": pull new detections, spill each to the firehose, escalate
// severe ones, then reschedule at the config cadence.
void IdsmDaemon::handle_info(const char* info, IdsmDaemonState& s) {
    if (!info || std::strcmp(info, "drain") != 0) return;

    bool any = false;
    // 1. the raw eBPF/mock backend (Cat B short-lived dials on a capable host).
    any |= process_detections_(*this, s, s.backend.poll());
    // 2. the userspace ss/proc sensor (Cat A/C/D/H — no eBPF needed), scoped to
    //    the Theia FCs (children of the supervisor).
    if (s.proc_scan)
        any |= process_detections_(*this, s, s.proc.scan(s.supervisor_pid));
    if (any) broadcast_status_(*this, s);

    uint32_t ms = s.poll_ms ? s.poll_ms : 500;
    ::theia::runtime::send_after(::theia::runtime::process_timers(), ms,
                                 *this, "drain");
}

// on_config_update: apply IdsmConfig live (re-open the backend if the source
// paths changed; update cadence + escalation threshold).
void IdsmDaemon::on_config_update(
        const platform_runtime_ConfigUpdated& cfg,
        IdsmDaemonState& s) {
    system_services_idsm_IdsmConfig c = system_services_idsm_IdsmConfig_init_zero;
    pb_istream_t is = pb_istream_from_buffer(
        reinterpret_cast<const pb_byte_t*>(cfg.config.bytes), cfg.config.size);
    if (!pb_decode(&is, system_services_idsm_IdsmConfig_fields, &c)) {
        log().warn("on_config_update: IdsmConfig decode failed — not applied");
        return;
    }
    const bool reopen = (s.bpf_object_path != c.bpf_object_path) ||
                        (s.mock_event_path != c.mock_event_path) ||
                        (s.ringbuf_map != c.ringbuf_map);
    s.bpf_object_path  = c.bpf_object_path;
    s.ringbuf_map      = c.ringbuf_map[0] ? c.ringbuf_map : "ids_events";
    s.poll_ms          = c.poll_ms ? c.poll_ms : 500;
    s.escalate_severity = c.escalate_severity;
    s.mock_event_path  = c.mock_event_path;
    if (reopen) reopen_backend_(s);
    // ProcBackend (Cat A/C/D/H) allow-lists.
    s.proc_scan         = c.proc_scan;
    s.expected_listeners = c.expected_listeners;
    s.grpc_servers      = c.grpc_servers;
    s.elf_digests       = c.elf_digests;
    apply_proc_config_(s);
    log().info(std::string("config: bpf='") + s.bpf_object_path + "' mock='" +
        s.mock_event_path + "' poll_ms=" + std::to_string(s.poll_ms) +
        " escalate>=" + std::to_string(s.escalate_severity) +
        " proc_scan=" + (s.proc_scan ? "on" : "off") +
        " expected_listeners='" + s.expected_listeners + "'");
    broadcast_status_(*this, s);
}

// GetIdsStatus — serve the current status snapshot.
IdsStatusMsg IdsmDaemon::handle_call(
        const IdsStatusReq& /*req*/,
        IdsmDaemonState& s) {
    IdsStatusMsg msg = system_services_idsm_IdsStatusMsg_init_zero;
    msg.state = static_cast<system_services_idsm_IdsState>(s.state);
    msg.on_ebpf = s.backend.on_ebpf();
    msg.events_total = s.events_total;
    msg.escalated_total = s.escalated_total;
    std::strncpy(msg.last_signature, s.last_signature.c_str(),
                 sizeof(msg.last_signature) - 1);
    msg.ts_ns = now_ns_();
    return msg;
}

}  // namespace ara::idsm
