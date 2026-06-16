// Standalone unit check for the IDSM ingest → TraceRecord-firehose pipeline.
//
// No TIPC / no stack: drives the mock IdsBackend against a temp newline-JSON
// file, runs the same encode+submit the handler does (via the TraceSubmitter
// test_sink seam so no AF_TIPC is needed), and asserts the firehose received a
// kind=SECURITY TraceRecord per mock detection. Proves the eBPF-degraded path
// (mock source) end-to-end on a host without eBPF.

#include "impl/ids_backend.hpp"
#include "Tracer.hh"
#include "system/services/log/log.pb.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ara::idsm;

// Mirror the handler's submit_detection_ (TraceRecord strings are pb_callback in
// nanopb, so we HAND-ENCODE proto3 wire, same as Tracer.hh + the handler).
static void pb_varint(std::string& o, uint64_t v) {
    while (v >= 0x80) { o += char((v & 0x7F) | 0x80); v >>= 7; } o += char(v);
}
static void pb_tag(std::string& o, uint32_t f, uint32_t w) {
    pb_varint(o, (uint64_t(f) << 3) | w);
}
static void pb_str(std::string& o, uint32_t f, const std::string& s) {
    if (s.empty()) return; pb_tag(o, f, 2); pb_varint(o, s.size()); o += s;
}
static void pb_vf(std::string& o, uint32_t f, uint64_t v) {
    if (!v) return; pb_tag(o, f, 0); pb_varint(o, v);
}
static void submit_detection(const DetectionEv& ev) {
    std::string rec;
    pb_str(rec, 1, "idsm");
    pb_str(rec, 2, ev.dst);
    pb_str(rec, 3, ev.signature);
    pb_vf(rec, 4, ev.severity);
    pb_vf(rec, 5, ev.ts_ns ? ev.ts_ns : 1);
    pb_vf(rec, 6, system_services_log_TraceKind_TraceKind_SECURITY);
    std::string p = ev.src + " -> " + ev.dst;
    if (!p.empty()) { pb_tag(rec, 7, 2); pb_varint(rec, p.size()); rec += p; }
    ::theia::runtime::TraceSubmitter::instance().submit(rec);
}

int main() {
    // 1. Write a mock event file with two detections (one severe).
    const char* path = "/tmp/idsm_test_events.json";
    {
        FILE* f = std::fopen(path, "w");
        assert(f);
        std::fprintf(f,
            "{\"severity\":2,\"signature\":\"port-scan\",\"src\":\"10.0.0.9:443\",\"dst\":\"local\",\"ts_ns\":111}\n");
        std::fprintf(f,
            "{\"severity\":5,\"signature\":\"spoofed-src\",\"src\":\"1.2.3.4\",\"dst\":\"eth0\",\"ts_ns\":222}\n");
        std::fclose(f);
    }

    // 2. Capture submitted firehose records via the test_sink seam.
    std::vector<std::string> submitted;
    ::theia::runtime::TraceSubmitter::instance().set_test_sink(
        [&](const std::string& w) { submitted.push_back(w); });

    // 3. Open the mock backend + drain.
    IdsBackend be;
    int st = be.open(/*bpf*/"", "ids_events", path);
    assert(st == I_LOADED && "mock source should load");
    assert(!be.on_ebpf() && "no real eBPF on this host");

    auto events = be.poll();
    assert(events.size() == 2 && "two detections drained");
    int severe = 0;
    for (auto& ev : events) { submit_detection(ev); if (ev.severity >= 4) ++severe; }
    assert(severe == 1 && "one severe (sev=5) detection");

    // 4. Assert the firehose received two SECURITY records. The TraceRecord
    //    strings are pb_callback (hand-encoded wire), so we check the wire
    //    bytes directly: every record carries node_name "idsm" + the
    //    kind=SECURITY (field 6 varint = 6), and one carries "spoofed-src".
    assert(submitted.size() == 2 && "two records hit the firehose");
    bool saw_spoof = false;
    const std::string kind6 = {char((6u << 3) | 0u), char(6)};  // tag(6,varint) + 6
    for (auto& w : submitted) {
        assert(w.find("idsm") != std::string::npos);
        assert(w.find(kind6) != std::string::npos && "kind=SECURITY on the wire");
        if (w.find("spoofed-src") != std::string::npos) saw_spoof = true;
    }
    assert(saw_spoof);

    // 5. A second poll yields nothing new (cursor advanced).
    assert(be.poll().empty() && "no new events after draining");

    std::printf("idsm ingest test: OK — 2 detections → 2 SECURITY firehose "
                "records, 1 severe, cursor advances\n");
    std::remove(path);
    return 0;
}
