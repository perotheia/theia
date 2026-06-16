// State struct for IdsmDaemon — APP-OWNED, WRITE-ONCE.
//
// Holds the IDS backend (the event source), the applied IdsmConfig, and the
// running counters the status snapshot reports. No kernel state is owned here —
// the eBPF program / ring buffer lives in the kernel; this is the FC's view.

#pragma once

#include <cstdint>
#include <string>

#include "ids_backend.hpp"   // IdsBackend / IState ordinals

namespace ara::idsm {

struct IdsmDaemonState {
    // The event source (eBPF ring buffer, or the mock file-tail fallback).
    IdsBackend  backend;
    int         state = I_UNAVAILABLE;   // IdsState ordinal

    // Applied config (from IdsmConfig / on_config_update).
    std::string bpf_object_path;
    std::string ringbuf_map      = "ids_events";
    uint32_t    poll_ms          = 500;
    uint32_t    escalate_severity = 4;
    std::string mock_event_path;

    // Running counters (the status snapshot).
    uint64_t    events_total    = 0;
    uint64_t    escalated_total = 0;
    std::string last_signature;
};

}  // namespace ara::idsm
