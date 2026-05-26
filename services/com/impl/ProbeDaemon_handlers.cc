// User handler bodies for ProbeDaemon.
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-app --kind fc` checks for
// this file's existence and refuses to overwrite unless `--force`
// is passed. Bodies are yours; the declarations are in
// lib/ProbeDaemon.hh.
//
// Default behaviour for every handler is a no-op (logs the message
// type to stderr so you can see traffic land). Replace with real
// behaviour as the FC matures.

#include "lib/ProbeDaemon.hh"

#include <cstdio>

namespace ara::com {


// ---- Fall-through receive (#409) ------------------------------------------
//
// The probe is a `fallthrough = true` node: TipcMux delivers here every
// inbound frame whose service_id matched no register_cast/register_call.
// That is exactly the probe's job — it impersonates and drives arbitrary
// nodes, so it accepts traffic the port schema doesn't name (a normal FC
// node would CRITICAL-error here; see GenServer's base handle_info).
//
// Runs on the probe's own GenServer thread. m.data is valid only for the
// duration of this call. Today we attribute + log the frame (it surfaces
// as a trace "unrouted" Info event for the reporting collector path too);
// future probe scenarios decode m by m.service_id and assert/forward. The
// active side — outbound inject / call — lives in robot_node.cpp, driven
// by the services-com gRPC bridge.
void ProbeDaemon::handle_info(const demo::runtime::InfoMsg& m,
                              ProbeDaemonState& /*s*/) {
    std::fprintf(stderr,
        "[%s] recv fall-through frame: service_id=0x%04X %s corr=%u len=%u "
        "(from peer; dst tipc=0x%08X)\n",
        kNodeName, m.service_id,
        (m.msg_type == 0x21u ? "CALL" : "CAST"),
        m.corr_id, m.len, m.dst_tipc_type);
}



ComEmpty ProbeDaemon::handle_call(
        const ComEmpty& /*req*/,
        ProbeDaemonState& /*s*/) {
    // TODO: implement Ping (ComEmpty →
    //                                 ComEmpty).
    // Dispatched by request type; one or more server ports may
    // expose this operation.
    std::fprintf(stderr, "[%s] Ping called\n",
                 kNodeName);
    return ComEmpty{};
}


}  // namespace ara::com
