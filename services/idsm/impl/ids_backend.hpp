// ids_backend — the intrusion-detection event source behind IdsmDaemon.
//
// APP-OWNED. The IDS engine runs in the kernel (eBPF); this backend only
// INGESTS its events. v1 (resolved decision: eBPF ring-buffer):
//
//   - eBPF path: load the compiled .bpf.o (config.bpf_object_path), attach it,
//     and drain its bpffs-pinned ring buffer (config.ringbuf_map) on each poll.
//     Requires libbpf + bpffs + CAP_BPF — ABSENT on the x86 dev host, so this
//     path is compiled-out behind THEIA_HAVE_LIBBPF (default off) and reports
//     IDS_UNAVAILABLE. It slots in on a capable host (Orin) without touching the
//     FC: only this header gains the libbpf calls.
//
//   - mock path (graceful degrade): when no eBPF object is loaded but
//     config.mock_event_path names a newline-JSON file, drain new lines from it
//     and parse each as a Detection. This exercises the full
//     ingest→normalize→firehose→escalate pipeline on a host without eBPF
//     (append a JSON line to the file → a detection flows), exactly like tsync's
//     mock backend without linuxptp.
//
// A Detection is engine-agnostic: {severity, signature, src, dst, ts_ns}. The FC
// turns it into a TraceRecord(kind=SECURITY) for the firehose.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ara::idsm {

// IdsState ordinals (.art): 0=UNAVAILABLE 1=LOADED 2=DEGRADED.
enum IState : int { I_UNAVAILABLE = 0, I_LOADED = 1, I_DEGRADED = 2 };

struct DetectionEv {
    uint32_t    severity = 1;     // 1=low … 5=critical
    std::string signature;
    std::string src;
    std::string dst;
    uint64_t    ts_ns = 0;
};

namespace ids_detail {

// Pull one JSON string field: "<key>":"<value>". Minimal (no nested escapes);
// enough for the flat detection records the mock/EVE sources emit.
inline std::string json_str_(const std::string& line, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    auto k = line.find(pat);
    if (k == std::string::npos) return "";
    auto colon = line.find(':', k + pat.size());
    if (colon == std::string::npos) return "";
    auto q1 = line.find('"', colon + 1);
    if (q1 == std::string::npos) return "";
    auto q2 = line.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return line.substr(q1 + 1, q2 - q1 - 1);
}

// Pull one JSON integer field: "<key>":<n>.
inline uint64_t json_int_(const std::string& line, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    auto k = line.find(pat);
    if (k == std::string::npos) return 0;
    auto colon = line.find(':', k + pat.size());
    if (colon == std::string::npos) return 0;
    return std::strtoull(line.c_str() + colon + 1, nullptr, 10);
}

}  // namespace ids_detail

// The backend: holds the open state + the mock file offset. Polling returns the
// new detections since the last poll. Stateful (the file cursor), so the FC owns
// one instance in its State.
class IdsBackend {
public:
    // Open the detector. Tries eBPF first (when an object path is set + libbpf is
    // compiled in); else falls back to a mock file source when one is configured;
    // else stays UNAVAILABLE. Idempotent-ish: call on init + on config change.
    int open(const std::string& bpf_object_path,
             const std::string& ringbuf_map,
             const std::string& mock_event_path) {
        ringbuf_map_ = ringbuf_map;
        mock_path_ = mock_event_path;
#ifdef THEIA_HAVE_LIBBPF
        if (!bpf_object_path.empty()) {
            // (Orin path) bpf_object__open + load + attach + ring_buffer__new on
            // the pinned `ringbuf_map`. Set on_ebpf_ = true on success.
            if (open_ebpf_(bpf_object_path)) { on_ebpf_ = true; return I_LOADED; }
        }
#else
        (void)bpf_object_path;
#endif
        // Degrade: a mock file source keeps the pipeline exercisable.
        if (!mock_path_.empty()) {
            mock_offset_ = 0;
            return I_LOADED;   // "loaded" via the mock source
        }
        return I_UNAVAILABLE;
    }

    bool on_ebpf() const { return on_ebpf_; }

    // Drain new detections since the last poll. eBPF → ring_buffer__poll; mock →
    // read new lines from the file past mock_offset_. Returns 0..N events.
    std::vector<DetectionEv> poll() {
        std::vector<DetectionEv> out;
#ifdef THEIA_HAVE_LIBBPF
        if (on_ebpf_) { drain_ebpf_(out); return out; }
#endif
        if (!mock_path_.empty()) drain_mock_(out);
        return out;
    }

private:
    bool        on_ebpf_ = false;
    std::string ringbuf_map_;
    std::string mock_path_;
    long        mock_offset_ = 0;

    // Read new newline-JSON lines from the mock file past mock_offset_, parse
    // each into a DetectionEv. Tolerates partial appends (re-reads next poll).
    void drain_mock_(std::vector<DetectionEv>& out) {
        FILE* f = ::fopen(mock_path_.c_str(), "r");
        if (!f) return;
        if (::fseek(f, mock_offset_, SEEK_SET) != 0) { ::fclose(f); return; }
        char buf[1024];
        while (::fgets(buf, sizeof(buf), f)) {
            size_t len = std::strlen(buf);
            if (len == 0 || buf[len - 1] != '\n') break;   // partial line; retry
            mock_offset_ += static_cast<long>(len);
            std::string line(buf, len - 1);
            if (line.empty()) continue;
            DetectionEv ev;
            ev.severity  = static_cast<uint32_t>(ids_detail::json_int_(line, "severity"));
            if (ev.severity == 0) ev.severity = 1;
            ev.signature = ids_detail::json_str_(line, "signature");
            ev.src       = ids_detail::json_str_(line, "src");
            ev.dst       = ids_detail::json_str_(line, "dst");
            ev.ts_ns     = ids_detail::json_int_(line, "ts_ns");
            if (ev.signature.empty()) ev.signature = "unknown";
            out.push_back(std::move(ev));
        }
        ::fclose(f);
    }

#ifdef THEIA_HAVE_LIBBPF
    bool open_ebpf_(const std::string& obj);     // defined in the Orin build
    void drain_ebpf_(std::vector<DetectionEv>&); //  ”
#endif
};

}  // namespace ara::idsm
