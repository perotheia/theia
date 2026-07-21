// deployment_resolver — InstanceSpecifier → iceoryx StreamSpec.
//
// RDS owns stream DISCOVERY: an application asks for a logical name
// ("/Perception/CameraFront") and the resolver maps it to the iceoryx
// {service, instance, event} triple + the pool sizing — the same way ara::com
// resolves a service deployment from the manifest. v1 uses a path convention
// (last two segments → service/instance, a fixed "frame" event) so a stream
// works without a manifest entry; a real deployment overrides chunk_size/history
// from the manifest `rds_stream` declaration (carried in StreamSpec by gen-fc).

#include "ara/rds/transport_if.h"

#include <string>
#include <vector>

namespace ara::rds {

namespace {
std::vector<std::string> split_(const std::string& s, char d) {
    std::vector<std::string> out; std::string cur;
    for (char c : s) { if (c == d) { if (!cur.empty()) out.push_back(cur); cur.clear(); }
                       else cur += c; }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
}  // namespace

// Resolve "/Perception/CameraFront" → service="Perception" instance="CameraFront"
// event="frame". chunk_size/history default here; the manifest (via gen-fc)
// fills the real pool sizing. iceoryx caps each id at 100 chars.
StreamSpec resolve_instance(const std::string& instance_specifier,
                            uint32_t max_chunk, uint32_t history) {
    StreamSpec s;
    auto parts = split_(instance_specifier, '/');
    if (parts.size() >= 2) {
        s.service  = parts[parts.size() - 2];
        s.instance = parts[parts.size() - 1];
    } else if (parts.size() == 1) {
        s.service  = "Rds";
        s.instance = parts[0];
    } else {
        s.service = "Rds"; s.instance = "default";
    }
    s.event     = "frame";
    s.max_chunk = max_chunk;
    s.history   = history;
    return s;
}

}  // namespace ara::rds
