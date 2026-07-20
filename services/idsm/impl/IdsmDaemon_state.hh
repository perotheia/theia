// State struct for IdsmDaemon — APP-OWNED, WRITE-ONCE.
//
// Holds the IDS backend (the event source), the applied IdsmConfig, and the
// running counters the status snapshot reports. No kernel state is owned here —
// the eBPF program / ring buffer lives in the kernel; this is the FC's view.

#pragma once

#include <cstdint>
#include <string>

#include "ids_backend.hpp"     // IdsBackend / IState ordinals
#include "proc_detector.hpp"   // ProcDetector — the userspace Cat A/C/D/H sensor

namespace ara::idsm {

struct IdsmDaemonState {
    // The event source (eBPF ring buffer, or the mock file-tail fallback).
    IdsBackend  backend;
    int         state = I_UNAVAILABLE;   // IdsState ordinal

    // The userspace ss/proc detector (Cat A/C/D/H — no eBPF). Runs alongside the
    // backend each tick; always available (no kernel sensor needed).
    ProcDetector proc;
    bool         proc_scan = true;
    int          supervisor_pid = -1;   // FC's parent = the supervisor (getppid)

    // Applied config (from IdsmConfig / on_config_update).
    std::string bpf_object_path;
    std::string ringbuf_map      = "ids_events";
    uint32_t    poll_ms          = 500;
    uint32_t    escalate_severity = 4;
    std::string mock_event_path;
    // ProcBackend defaults mirror the .art IdsmConfig defaults so boot-time
    // scanning is correct before the first etcd config push.
    std::string expected_listeners = "com:7700,com:7710,com:7711";
    std::string grpc_servers       = "com";
    std::string elf_digests;
    // The supervised FC set (comma-separated comm) — manifest-derived via config,
    // NOT hardcoded. Classifies Cat A (real FC, wrong port) vs Cat C (rogue). The
    // default mirrors the .art IdsmConfig default for boot-time correctness.
    std::string known_fcs =
        "com,crypto,log,nm,osi,per,sm,tsync,ucm,shwa,fw,idsm,p1,p2,p3,p4";

    // Running counters (the status snapshot).
    uint64_t    events_total    = 0;
    uint64_t    escalated_total = 0;
    std::string last_signature;

    // PHM health edge-latch: last detector-health level reported (-1 = none yet),
    // so broadcast_status_ escalates only on a level CHANGE (it runs every poll).
    int         last_health     = -1;
};

}  // namespace ara::idsm
